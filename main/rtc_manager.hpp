#pragma once

#include <stdint.h>

#include "board_config.hpp"
#include "esp_err.h"

struct RtcDateTime {
    // The RTC stores a UTC calendar value. The PCF85063A has no timezone or
    // daylight-saving state; callers that need civil time must apply their
    // configured offset to these fields.
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t weekday = 0;  // 0=Sunday
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    bool oscillator_stopped = false;
};

struct RtcStatus {
    bool present = false;
    bool initialized = false;
    esp_err_t initialization_result = ESP_ERR_NOT_SUPPORTED;
};

esp_err_t rtc_manager_start(const BoardConfig &board);
esp_err_t rtc_manager_get_status(RtcStatus *status);
esp_err_t rtc_manager_read(RtcDateTime *value);
esp_err_t rtc_manager_set(const RtcDateTime &value);
bool rtc_datetime_valid(const RtcDateTime &value);
uint8_t rtc_weekday(uint16_t year, uint8_t month, uint8_t day);
