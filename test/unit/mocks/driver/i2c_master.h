/* Host test mock of driver/i2c_master.h (new ESP-IDF I2C master driver).
 * Provides just enough of the API surface for battery_mgr.cpp to compile and
 * be exercised natively. Test-controllable behavior lives in stubs_esp.cpp:
 *   - i2c_mock_reset(): reset all mock state
 *   - i2c_mock_set_probe_result(esp_err_t): what i2c_master_probe returns
 *   - i2c_mock_set_reg(uint8_t reg, uint8_t val): canned register byte the
 *     device returns for a given register address on transmit_receive
 *   - i2c_mock_set_transfer_result(esp_err_t): force transmit_receive to fail
 */
#ifndef MOCK_DRIVER_I2C_MASTER_H
#define MOCK_DRIVER_I2C_MASTER_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;

typedef enum {
    I2C_ADDR_BIT_LEN_7 = 0,
    I2C_ADDR_BIT_LEN_10 = 1,
} i2c_addr_bit_len_t;

typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
                           int xfer_timeout_ms);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *dev_config,
                                    i2c_master_dev_handle_t *ret_handle);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
                                      const uint8_t *write_buffer,
                                      size_t write_size, uint8_t *read_buffer,
                                      size_t read_size, int xfer_timeout_ms);

/* Test control hooks (defined in stubs_esp.cpp). */
void i2c_mock_reset(void);
void i2c_mock_set_probe_result(esp_err_t result);
void i2c_mock_set_reg(uint8_t reg, uint8_t val);
void i2c_mock_set_transfer_result(esp_err_t result);

#ifdef __cplusplus
}
#endif

#endif
