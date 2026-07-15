/* Unit tests for main/battery_mgr.cpp (STC8H companion-MCU battery monitor).
 *
 * The parsing logic (register-block byte offsets, level clamping, charge-state
 * classification) is the part most prone to silent bugs (a wrong offset once
 * made the on-screen icon rotate between charging/100%/0%). We white-box the
 * source so the file-static battery_parse_block() is reachable, and also drive
 * the public API end-to-end against the host I2C mock (mocks/driver/i2c_master.h
 * + stubs_esp.cpp), which emulates the STC8H's single-byte register reads.
 *
 * Compiled with CONFIG_IDF_TARGET_ESP32P4 so the driver's full path is active. */
#include "gtest/gtest.h"

#include "driver/i2c_master.h"

// White-box include: pulls in the static battery_parse_block() and the file's
// state so we can test both the pure parser and the public API in-process.
#include "../../main/battery_mgr.cpp"

namespace {

// Build a 12-byte STC8H block: [0..3]=adc_voltage, [4..7]=bat_voltage,
// [8]=level, [9]=state, [10]=led, [11]=pad (all little-endian).
void make_block(uint8_t buf[12], uint32_t adc_mv, uint32_t bat_mv,
                uint8_t level, uint8_t state, uint8_t led) {
    memset(buf, 0, 12);
    buf[0] = adc_mv & 0xFF;  buf[1] = (adc_mv >> 8) & 0xFF;
    buf[2] = (adc_mv >> 16) & 0xFF; buf[3] = (adc_mv >> 24) & 0xFF;
    buf[4] = bat_mv & 0xFF;  buf[5] = (bat_mv >> 8) & 0xFF;
    buf[6] = (bat_mv >> 16) & 0xFF; buf[7] = (bat_mv >> 24) & 0xFF;
    buf[8] = level;
    buf[9] = state;
    buf[10] = led;
}

// --------------------------- pure parser ---------------------------

TEST(BatteryParse, ExtractsVoltageLevelAndState) {
    uint8_t buf[12];
    make_block(buf, /*adc*/ 4200, /*bat*/ 3950, /*level*/ 87,
               /*state*/ BATTERY_CHARGE_NO_CHARGE, /*led*/ 3);
    battery_status_t s;
    battery_parse_block(buf, &s);
    EXPECT_TRUE(s.valid);
    EXPECT_EQ(s.bat_voltage_mv, 3950u);       // from bytes [4..7], not adc
    EXPECT_EQ(s.level_percent, 87);
    EXPECT_EQ(s.state, BATTERY_CHARGE_NO_CHARGE);
}

TEST(BatteryParse, UsesBatVoltageOffsetNotAdc) {
    // Distinct adc vs bat voltage proves we read the correct offset (regression
    // guard for the byte-offset bug).
    uint8_t buf[12];
    make_block(buf, 5000, 4100, 50, BATTERY_CHARGE_CHARGING, 0);
    battery_status_t s;
    battery_parse_block(buf, &s);
    EXPECT_EQ(s.bat_voltage_mv, 4100u);
}

TEST(BatteryParse, ClampsLevelAbove100) {
    uint8_t buf[12];
    make_block(buf, 0, 4200, 200 /*bogus*/, BATTERY_CHARGE_IDLE, 0);
    battery_status_t s;
    battery_parse_block(buf, &s);
    EXPECT_EQ(s.level_percent, 100);
}

TEST(BatteryParse, CoercesOutOfRangeStateToError) {
    uint8_t buf[12];
    make_block(buf, 0, 4200, 50, 0xFF /*bogus state*/, 0);
    battery_status_t s;
    battery_parse_block(buf, &s);
    EXPECT_EQ(s.state, BATTERY_CHARGE_ERROR);
}

TEST(BatteryParse, ChargingStateParsed) {
    uint8_t buf[12];
    make_block(buf, 0, 4200, 100, BATTERY_CHARGE_CHARGING, 0);
    battery_status_t s;
    battery_parse_block(buf, &s);
    EXPECT_EQ(s.state, BATTERY_CHARGE_CHARGING);
}

// --------------------------- charging predicate ---------------------

TEST(BatteryCharging, ChargingAndFullCountAsCharging) {
    EXPECT_TRUE(battery_mgr_state_is_charging(BATTERY_CHARGE_CHARGING));
    EXPECT_TRUE(battery_mgr_state_is_charging(BATTERY_CHARGE_FULLY_CHARGED));
}

TEST(BatteryCharging, DischargingStatesAreNotCharging) {
    EXPECT_FALSE(battery_mgr_state_is_charging(BATTERY_CHARGE_IDLE));
    EXPECT_FALSE(battery_mgr_state_is_charging(BATTERY_CHARGE_NO_CHARGE));
    EXPECT_FALSE(battery_mgr_state_is_charging(BATTERY_CHARGE_ERROR));
}

// --------------------------- public API (via I2C mock) --------------

class BatteryApi : public ::testing::Test {
protected:
    void SetUp() override {
        i2c_mock_reset();
        // Reset the driver's file-static device handle between tests so
        // battery_mgr_init runs its full path each time.
        s_dev = nullptr;
    }
};

TEST_F(BatteryApi, InitFailsWhenDeviceAbsent) {
    i2c_mock_set_probe_result(ESP_ERR_TIMEOUT);  // STC8H not acking
    EXPECT_NE(battery_mgr_init((i2c_master_bus_handle_t)1), ESP_OK);
    EXPECT_FALSE(battery_mgr_available());
}

TEST_F(BatteryApi, InitRejectsNullBus) {
    EXPECT_EQ(battery_mgr_init(nullptr), ESP_ERR_INVALID_ARG);
}

TEST_F(BatteryApi, ReadWithoutInitReturnsInvalidState) {
    battery_status_t s;
    EXPECT_EQ(battery_mgr_read(&s), ESP_ERR_INVALID_STATE);
}

TEST_F(BatteryApi, InitPrimesCacheFromRegisters) {
    // Program the STC8H register table: level=64%, charging, bat=3900mV.
    uint8_t buf[12];
    make_block(buf, 4000, 3900, 64, BATTERY_CHARGE_CHARGING, 2);
    for (uint8_t i = 0; i < 12; i++) i2c_mock_set_reg(i, buf[i]);

    ASSERT_EQ(battery_mgr_init((i2c_master_bus_handle_t)1), ESP_OK);
    EXPECT_TRUE(battery_mgr_available());

    // init primes the cache with one read; the public read returns it.
    battery_status_t s;
    ASSERT_EQ(battery_mgr_read(&s), ESP_OK);
    EXPECT_TRUE(s.valid);
    EXPECT_EQ(s.level_percent, 64);
    EXPECT_EQ(s.bat_voltage_mv, 3900u);
    EXPECT_TRUE(battery_mgr_state_is_charging(s.state));
}

TEST_F(BatteryApi, ReadReflectsRegisterValues) {
    uint8_t buf[12];
    make_block(buf, 0, 3300, 5, BATTERY_CHARGE_NO_CHARGE, 4);  // low battery
    for (uint8_t i = 0; i < 12; i++) i2c_mock_set_reg(i, buf[i]);
    ASSERT_EQ(battery_mgr_init((i2c_master_bus_handle_t)1), ESP_OK);

    battery_status_t s;
    ASSERT_EQ(battery_mgr_read(&s), ESP_OK);
    EXPECT_EQ(s.level_percent, 5);
    EXPECT_FALSE(battery_mgr_state_is_charging(s.state));
}

}  // namespace
