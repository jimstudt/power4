#pragma once

#include <stdint.h>

#include "esp_err.h"

struct RelayStatus {
    uint8_t relay = 0;
    int gpio_pin = -1;
    uint8_t active_level = 1;
    bool timer_active = false;
    bool forced_on = false;
    bool output_on = false;
    uint32_t timer_remaining_s = 0;
};

uint8_t relay_manager_count(void);
esp_err_t relay_manager_start(void);
esp_err_t relay_manager_on_for(uint8_t relay, uint32_t seconds);
esp_err_t relay_manager_force_on(uint8_t relay);
esp_err_t relay_manager_clear_force(uint8_t relay);
esp_err_t relay_manager_query(uint8_t relay, RelayStatus *status);
