#pragma once

#include <stdint.h>

#include "esp_err.h"

// Administrative override. Force-off wins over everything, including an
// active policy timer; force-on holds the relay closed with no timer.
enum class RelayForce : uint8_t {
    None,
    On,
    Off,
};

struct RelayStatus {
    uint8_t relay = 0;
    int gpio_pin = -1;
    uint8_t active_level = 1;
    bool timer_active = false;
    RelayForce force = RelayForce::None;
    bool output_on = false;
    uint32_t timer_remaining_s = 0;
};

const char *relay_force_name(RelayForce force);

uint8_t relay_manager_count(void);
esp_err_t relay_manager_start(void);
esp_err_t relay_manager_on_for(uint8_t relay, uint32_t seconds);
esp_err_t relay_manager_force_on(uint8_t relay);
esp_err_t relay_manager_force_off(uint8_t relay);
esp_err_t relay_manager_clear_force(uint8_t relay);
esp_err_t relay_manager_query(uint8_t relay, RelayStatus *status);
