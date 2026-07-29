#pragma once

#include <stdint.h>

#include "board_config.hpp"
#include "esp_err.h"

struct InputManagerStatus {
    bool present = false;
    bool initialized = false;
    esp_err_t initialization_result = ESP_ERR_NOT_SUPPORTED;
    uint8_t count = 0;
};

struct InputStatus {
    uint8_t input = 0;
    DigitalInputBackendKind backend = DigitalInputBackendKind::None;
    int gpio_pin = -1;
    uint8_t active_level = 0;
    int level = 0;
    bool on = false;
};

esp_err_t input_manager_start(const BoardConfig &board);
esp_err_t input_manager_get_status(InputManagerStatus *status);
uint8_t input_manager_count(void);
esp_err_t input_manager_query(uint8_t input, InputStatus *status);
