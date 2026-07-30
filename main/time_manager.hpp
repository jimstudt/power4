#pragma once

#include <stddef.h>
#include <time.h>

#include "esp_err.h"
#include "rtc_manager.hpp"

constexpr size_t kTimezoneNameBytes = 24;
constexpr size_t kPosixTimezoneBytes = 48;
constexpr size_t kTimezoneAbbreviationBytes = 8;

struct TimezoneInfo {
    const char *human_name;
    const char *posix_rule;
    const char *short_name;
};

enum class TimeSource {
    Unset,
    Rtc,
    Manual,
    Sntp,
};

struct TimeManagerStatus {
    bool clock_valid = false;
    bool sntp_started = false;
    TimeSource source = TimeSource::Unset;
    time_t last_sntp_sync = 0;
    char timezone_name[kTimezoneNameBytes] = {};
    char posix_timezone[kPosixTimezoneBytes] = {};
    char current_abbreviation[kTimezoneAbbreviationBytes] = {};
};

struct TimeSnapshot {
    bool valid = false;
    time_t epoch = 0;
    struct tm utc = {};
    struct tm local = {};
    int utc_offset_minutes = 0;
    char timezone_name[kTimezoneNameBytes] = {};
    char abbreviation[kTimezoneAbbreviationBytes] = {};
    TimeSource source = TimeSource::Unset;
};

esp_err_t time_manager_init(void);
esp_err_t time_manager_seed_from_rtc(void);
esp_err_t time_manager_start_sntp(void);
esp_err_t time_manager_set_utc(const RtcDateTime &value);
esp_err_t time_manager_set_timezone(const char *name);
esp_err_t time_manager_get_status(TimeManagerStatus *status);
esp_err_t time_manager_get_time(TimeSnapshot *snapshot);

const char *time_source_name(TimeSource source);
const char *time_manager_sntp_server(void);
size_t time_manager_timezone_count(void);
const TimezoneInfo *time_manager_timezone_at(size_t index);
