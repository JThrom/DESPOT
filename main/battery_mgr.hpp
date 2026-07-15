#ifndef BATTERY_MGR_HPP
#define BATTERY_MGR_HPP

#include <stdint.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Battery monitoring for the Elecrow CrowPanel Advanced 9" (ESP32-P4).
//
// The board carries an STC8H1Kxx companion MCU on the shared I2C bus
// (address 0x2F, same SDA45/SCL46 bus as the GT911 touch controller). The
// STC8H performs battery ADC sampling and charge management and exposes the
// result as a register block starting at 0x00, matching the layout below.
// This mirrors bsp_stc8h1kxx.* in Elecrow's factory firmware.
//
// No extra hardware is required: the companion MCU is already populated and
// the battery-sense/charger circuitry is on-board.

// Charge state reported by the STC8H (bat_state register byte).
typedef enum {
    BATTERY_CHARGE_IDLE = 0,
    BATTERY_CHARGE_CHARGING,       // actively charging
    BATTERY_CHARGE_FULLY_CHARGED,  // charge complete
    BATTERY_CHARGE_NO_CHARGE,      // running on battery (no charger)
    BATTERY_CHARGE_ERROR,          // fault
} battery_charge_state_t;

typedef struct {
    bool valid;                    // false if the last read failed
    uint8_t level_percent;         // 0..100 remaining capacity
    battery_charge_state_t state;  // charge/discharge state
    uint32_t bat_voltage_mv;       // battery terminal voltage (mV)
} battery_status_t;

// Register the STC8H companion MCU on an already-created I2C master bus.
// `bus` is the shared touch/board I2C bus handle. Returns ESP_OK on success.
// Safe to call once; subsequent calls are no-ops once registered.
esp_err_t battery_mgr_init(i2c_master_bus_handle_t bus);

// True once battery_mgr_init has registered the companion MCU successfully.
bool battery_mgr_available(void);

// Read the current battery status. Returns ESP_OK and populates `out` on a
// successful I2C read; on failure `out->valid` is set false and an error is
// returned. If the manager was never initialized, returns ESP_ERR_INVALID_STATE.
esp_err_t battery_mgr_read(battery_status_t *out);

// Convenience: true when the reported state means the pack is charging (or the
// charger is attached and the pack is full).
bool battery_mgr_state_is_charging(battery_charge_state_t state);

#ifdef __cplusplus
}
#endif

#endif
