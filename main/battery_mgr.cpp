#include "battery_mgr.hpp"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "battery";

// STC8H companion MCU (Elecrow CrowPanel Advanced 9").
#define STC8H_I2C_ADDRESS 0x2F
#define STC8H_REG_BATTERY 0x00

// Register->struct-byte mapping of the STC8H battery block, matching Elecrow's
// Battery_info_t in bsp_stc8h1kxx.h. IMPORTANT: the STC8H does not support I2C
// auto-increment block reads; the factory driver reads each byte with its own
// register address (STC8_REG_ADDR_BATTERY + i) and stores it at struct byte i,
// padding included. Battery_info_t = { u32 adc_voltage; u32 bat_voltage;
// u8 bat_level; u8 bat_state; u8 led_state; } which the compiler aligns to
// 12 bytes:
//   [0..3]  adc_voltage (mV)
//   [4..7]  bat_voltage (mV)
//   [8]     bat_level   (%)
//   [9]     bat_state   (EM_BAT_CHARGE_STATE)
//   [10]    led_state
//   [11]    padding
#define STC8H_BATTERY_BLOCK_LEN 12
#define STC8H_OFF_BAT_VOLTAGE 4
#define STC8H_OFF_BAT_LEVEL 8
#define STC8H_OFF_BAT_STATE 9

// The STC8H shares I2C_NUM_0 with the GT911 touch controller, which is polled
// from the LVGL/main-loop context. To avoid adding a blocking I2C transaction
// to that hot path (which starves the BLE-over-esp_hosted pump and can corrupt
// the NimBLE mempool), all battery I2C traffic happens in this dedicated
// low-priority task once per second, mirroring Elecrow's factory firmware. The
// UI reads only the cached snapshot, which is guarded by a short mutex.

#define BATTERY_POLL_PERIOD_MS 1000
#define BATTERY_I2C_TIMEOUT_MS 200

static i2c_master_dev_handle_t s_dev = NULL;
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;
static battery_status_t s_cache = {};

static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// Parse a 12-byte STC8H battery register block into `out` (marks it valid).
// Pure logic (no I2C/RTOS), separated so it can be unit-tested directly. The
// battery level is clamped to 100 and an out-of-range state byte is coerced to
// BATTERY_CHARGE_ERROR.
static void battery_parse_block(const uint8_t buf[STC8H_BATTERY_BLOCK_LEN],
                                battery_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->bat_voltage_mv = rd_u32_le(&buf[STC8H_OFF_BAT_VOLTAGE]);
    uint8_t level = buf[STC8H_OFF_BAT_LEVEL];
    uint8_t state = buf[STC8H_OFF_BAT_STATE];
    if (level > 100) level = 100;
    out->level_percent = level;
    out->state = (state > BATTERY_CHARGE_ERROR) ? BATTERY_CHARGE_ERROR
                                                 : (battery_charge_state_t)state;
    out->valid = true;
}

// Perform one blocking I2C read and parse into `out`. Runs only in the poll
// task. Returns ESP_OK on success.
static esp_err_t battery_read_raw(battery_status_t *out) {
    memset(out, 0, sizeof(*out));
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    // Read each register byte individually (no auto-increment on the STC8H).
    uint8_t buf[STC8H_BATTERY_BLOCK_LEN] = {0};
    for (int i = 0; i < STC8H_BATTERY_BLOCK_LEN; i++) {
        uint8_t reg = (uint8_t)(STC8H_REG_BATTERY + i);
        esp_err_t err = i2c_master_transmit_receive(
            s_dev, &reg, 1, &buf[i], 1, BATTERY_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            out->valid = false;
            return err;
        }
    }

    battery_parse_block(buf, out);
    return ESP_OK;
}

static void battery_task(void *arg) {
    (void)arg;
    while (true) {
        battery_status_t s = {};
        esp_err_t err = battery_read_raw(&s);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(err));
        }
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_cache = s;
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(BATTERY_POLL_PERIOD_MS));
    }
}

esp_err_t battery_mgr_init(i2c_master_bus_handle_t bus) {
    if (s_dev) return ESP_OK;
    if (!bus) return ESP_ERR_INVALID_ARG;

    // Confirm the companion MCU actually acks before adding it.
    esp_err_t err = i2c_master_probe(bus, STC8H_I2C_ADDRESS, 50);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STC8H not present at 0x%02X: %s", STC8H_I2C_ADDRESS,
                 esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = STC8H_I2C_ADDRESS;
    cfg.scl_speed_hz = 100000;

    err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add STC8H device failed: %s", esp_err_to_name(err));
        s_dev = NULL;
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "battery mutex alloc failed");
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Prime the cache once so the first UI update has data.
    battery_status_t first = {};
    battery_read_raw(&first);
    s_cache = first;

    if (xTaskCreate(battery_task, "battery", 3072, NULL, 2, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "battery task create failed");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "STC8H battery monitor ready at 0x%02X", STC8H_I2C_ADDRESS);
    return ESP_OK;
}

bool battery_mgr_available(void) { return s_dev != NULL; }

bool battery_mgr_state_is_charging(battery_charge_state_t state) {
    return state == BATTERY_CHARGE_CHARGING ||
           state == BATTERY_CHARGE_FULLY_CHARGED;
}

esp_err_t battery_mgr_read(battery_status_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!s_dev || !s_lock) return ESP_ERR_INVALID_STATE;

    // Non-blocking-ish snapshot read of the cached value. No I2C here.
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out = s_cache;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
