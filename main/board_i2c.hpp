#pragma once

#include "board_config.hpp"
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t board_i2c_start(const I2cHardwareConfig &config);
esp_err_t board_i2c_add_device(uint8_t address,
                               uint32_t frequency_hz,
                               i2c_master_dev_handle_t *device);
bool board_i2c_started(void);
