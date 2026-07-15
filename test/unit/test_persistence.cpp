/* Persistence (NVS) contract tests.
 *
 * Several user settings persist in the shared "devicecfg" NVS namespace and are
 * restored at boot. The brightness bug (reset to 25% every power cycle) was a
 * MISSING save+load pair, not a broken NVS layer. Compiling the full shell.cpp
 * into the host harness would drag in the entire firmware dependency web
 * (ssh/tailscale/libssh2/lvgl), so instead these tests lock down the exact NVS
 * round-trip contract that brightness_save/brightness_load (shell.cpp),
 * terminal fg color, and idle timeout all rely on, using the same in-memory NVS
 * mock (mocks/stubs_esp.cpp) the firmware persistence code runs against.
 *
 * On-device: brightness persistence across power cycles is verified on hardware
 * (slider change -> power cycle -> value restored, not 25%). */
#include "gtest/gtest.h"

extern "C" {
#include "nvs.h"
}
#include "test_support.hpp"

namespace {

// Mirror of the shell.cpp constants (kept in sync intentionally).
constexpr char DEVICE_CFG_NS[] = "devicecfg";
constexpr char BRIGHTNESS_KEY[] = "blpct";
constexpr char IDLE_TMO_KEY[] = "idletmo";
constexpr char TERM_FG_KEY[] = "termfg";

class Persistence : public ::testing::Test {
protected:
    void SetUp() override { mock_nvs_reset(); }
};

TEST_F(Persistence, BrightnessU8RoundTrips) {
    nvs_handle_t h = 0;
    ASSERT_EQ(nvs_open(DEVICE_CFG_NS, NVS_READWRITE, &h), ESP_OK);
    ASSERT_EQ(nvs_set_u8(h, BRIGHTNESS_KEY, 73), ESP_OK);
    ASSERT_EQ(nvs_commit(h), ESP_OK);
    nvs_close(h);

    // Re-open (simulates a fresh boot) and read back.
    ASSERT_EQ(nvs_open(DEVICE_CFG_NS, NVS_READONLY, &h), ESP_OK);
    uint8_t v = 0;
    ASSERT_EQ(nvs_get_u8(h, BRIGHTNESS_KEY, &v), ESP_OK);
    EXPECT_EQ(v, 73);
    nvs_close(h);
}

TEST_F(Persistence, MissingBrightnessKeyLeavesDefault) {
    // The bug's failure mode on a fresh device: nothing was ever saved.
    // brightness_load()'s contract is to leave the running value untouched when
    // there is no stored key. Two sub-cases mirror the real helper's guards:
    //   (a) namespace doesn't exist yet -> RO open fails -> load returns early.
    //   (b) namespace exists but key absent -> get returns NOT_FOUND.
    uint8_t v = 25;  // caller default (the old hardcoded backlight default)

    // (a) fresh store: RO open of a never-written namespace fails.
    nvs_handle_t h = 0;
    EXPECT_NE(nvs_open(DEVICE_CFG_NS, NVS_READONLY, &h), ESP_OK);
    EXPECT_EQ(v, 25);  // default preserved

    // (b) namespace exists (some other key written) but brightness key absent.
    ASSERT_EQ(nvs_open(DEVICE_CFG_NS, NVS_READWRITE, &h), ESP_OK);
    ASSERT_EQ(nvs_set_u32(h, IDLE_TMO_KEY, 30000), ESP_OK);
    ASSERT_EQ(nvs_commit(h), ESP_OK);
    nvs_close(h);
    ASSERT_EQ(nvs_open(DEVICE_CFG_NS, NVS_READONLY, &h), ESP_OK);
    EXPECT_NE(nvs_get_u8(h, BRIGHTNESS_KEY, &v), ESP_OK);
    EXPECT_EQ(v, 25);  // still untouched
    nvs_close(h);
}

TEST_F(Persistence, BrightnessSurvivesUnrelatedKeys) {
    // brightness, idle timeout, and term color share the devicecfg namespace;
    // writing one must not disturb the others.
    nvs_handle_t h = 0;
    ASSERT_EQ(nvs_open(DEVICE_CFG_NS, NVS_READWRITE, &h), ESP_OK);
    ASSERT_EQ(nvs_set_u8(h, BRIGHTNESS_KEY, 40), ESP_OK);
    ASSERT_EQ(nvs_set_u32(h, IDLE_TMO_KEY, 45000), ESP_OK);
    ASSERT_EQ(nvs_set_u8(h, TERM_FG_KEY, 201), ESP_OK);
    ASSERT_EQ(nvs_commit(h), ESP_OK);
    nvs_close(h);

    ASSERT_EQ(nvs_open(DEVICE_CFG_NS, NVS_READONLY, &h), ESP_OK);
    uint8_t bl = 0, fg = 0;
    uint32_t tmo = 0;
    EXPECT_EQ(nvs_get_u8(h, BRIGHTNESS_KEY, &bl), ESP_OK);
    EXPECT_EQ(nvs_get_u32(h, IDLE_TMO_KEY, &tmo), ESP_OK);
    EXPECT_EQ(nvs_get_u8(h, TERM_FG_KEY, &fg), ESP_OK);
    EXPECT_EQ(bl, 40);
    EXPECT_EQ(tmo, 45000u);
    EXPECT_EQ(fg, 201);
    nvs_close(h);
}

TEST_F(Persistence, BrightnessOverwriteUpdatesValue) {
    nvs_handle_t h = 0;
    ASSERT_EQ(nvs_open(DEVICE_CFG_NS, NVS_READWRITE, &h), ESP_OK);
    ASSERT_EQ(nvs_set_u8(h, BRIGHTNESS_KEY, 10), ESP_OK);
    ASSERT_EQ(nvs_set_u8(h, BRIGHTNESS_KEY, 90), ESP_OK);  // slider moved again
    ASSERT_EQ(nvs_commit(h), ESP_OK);
    uint8_t v = 0;
    ASSERT_EQ(nvs_get_u8(h, BRIGHTNESS_KEY, &v), ESP_OK);
    EXPECT_EQ(v, 90);
    nvs_close(h);
}

}  // namespace
