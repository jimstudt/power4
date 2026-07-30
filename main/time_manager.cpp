#include "time_manager.hpp"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

namespace {

constexpr const char *kTag = "time_manager";
constexpr const char *kNamespace = "time";
constexpr const char *kTimezoneKey = "timezone";
constexpr const char *kDefaultTimezone = "UTC";
constexpr const char *kSntpServer = "162.159.200.123";
constexpr uint32_t kSyncTaskStackBytes = 4096;
constexpr UBaseType_t kSyncTaskPriority = 4;

constexpr TimezoneInfo kTimezones[] = {
    {"UTC", "UTC0", "UTC"},
    {"US/Hawaii", "HST10", "HST"},
    {"US/Alaska", "AKST9AKDT,M3.2.0/2,M11.1.0/2", "AKST"},
    {"US/Pacific", "PST8PDT,M3.2.0/2,M11.1.0/2", "PST"},
    {"US/Mountain", "MST7MDT,M3.2.0/2,M11.1.0/2", "MST"},
    {"US/Arizona", "MST7", "MST"},
    {"US/Central", "CST6CDT,M3.2.0/2,M11.1.0/2", "CST"},
    {"Mexico/Central", "CST6", "CST"},
    {"US/Eastern", "EST5EDT,M3.2.0/2,M11.1.0/2", "EST"},
    {"Canada/Atlantic", "AST4ADT,M3.2.0/2,M11.1.0/2", "AST"},
    {"Colombia", "COT5", "COT"},
    {"Argentina", "ART3", "ART"},
    {"Brazil/East", "BRT3", "BRT"},
    {"UK", "GMT0BST,M3.5.0/1,M10.5.0/2", "GMT"},
    {"Europe/Central", "CET-1CEST,M3.5.0/2,M10.5.0/3", "CET"},
    {"Europe/Eastern", "EET-2EEST,M3.5.0/3,M10.5.0/4", "EET"},
    {"SouthAfrica", "SAST-2", "SAST"},
    {"EastAfrica", "EAT-3", "EAT"},
    {"Russia/Moscow", "MSK-3", "MSK"},
    {"Gulf", "GST-4", "GST"},
    {"Pakistan", "PKT-5", "PKT"},
    {"India", "IST-5:30", "IST"},
    {"Bangladesh", "BST-6", "BST"},
    {"Indochina", "ICT-7", "ICT"},
    {"China", "CST-8", "CST"},
    {"Japan", "JST-9", "JST"},
    {"Australia/West", "AWST-8", "AWST"},
    {"Australia/Central", "ACST-9:30ACDT,M10.1.0/2,M4.1.0/3", "ACST"},
    {"Australia/East", "AEST-10AEDT,M10.1.0/2,M4.1.0/3", "AEST"},
    {"NewZealand", "NZST-12NZDT,M9.5.0/2,M4.1.0/3", "NZST"},
};

struct SyncMessage {
    time_t seconds;
};

SemaphoreHandle_t g_mutex = nullptr;
StaticSemaphore_t g_mutex_storage = {};
char g_timezone_name[kTimezoneNameBytes] = {};
char g_posix_timezone[kPosixTimezoneBytes] = {};
bool g_clock_valid = false;
bool g_sntp_started = false;
TimeSource g_source = TimeSource::Unset;
time_t g_last_sntp_sync = 0;

QueueHandle_t g_sync_queue = nullptr;
StaticQueue_t g_sync_queue_storage = {};
uint8_t g_sync_queue_buffer[sizeof(SyncMessage)] = {};
TaskHandle_t g_sync_task = nullptr;

const TimezoneInfo *find_timezone(const char *name)
{
    if (name == nullptr) {
        return nullptr;
    }
    for (const auto &timezone : kTimezones) {
        if (strcmp(name, timezone.human_name) == 0) {
            return &timezone;
        }
    }
    return nullptr;
}

esp_err_t apply_timezone_locked(const TimezoneInfo &timezone)
{
    if (setenv("TZ", timezone.posix_rule, 1) != 0) {
        return ESP_ERR_NO_MEM;
    }
    tzset();
    strlcpy(g_timezone_name, timezone.human_name, sizeof(g_timezone_name));
    strlcpy(g_posix_timezone, timezone.posix_rule, sizeof(g_posix_timezone));
    return ESP_OK;
}

esp_err_t load_timezone(char *name, size_t capacity)
{
    strlcpy(name, kDefaultTimezone, capacity);
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t length = capacity;
    err = nvs_get_str(handle, kTimezoneKey, name, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strlcpy(name, kDefaultTimezone, capacity);
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH || err == ESP_ERR_NVS_TYPE_MISMATCH) {
        strlcpy(name, kDefaultTimezone, capacity);
        return ESP_OK;
    }
    return err;
}

esp_err_t save_timezone(const char *name)
{
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(kNamespace, NVS_READWRITE, &handle),
                        kTag,
                        "failed to open time settings");
    esp_err_t err = nvs_set_str(handle, kTimezoneKey, name);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + day_of_era - 719468;
}

int64_t tm_as_utc_seconds(const struct tm &value)
{
    const int64_t days =
        days_from_civil(value.tm_year + 1900,
                        static_cast<unsigned>(value.tm_mon + 1),
                        static_cast<unsigned>(value.tm_mday));
    return days * 86400 +
           static_cast<int64_t>(value.tm_hour) * 3600 +
           static_cast<int64_t>(value.tm_min) * 60 +
           value.tm_sec;
}

esp_err_t rtc_to_epoch(const RtcDateTime &value, time_t *epoch)
{
    ESP_RETURN_ON_FALSE(epoch != nullptr && rtc_datetime_valid(value),
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "invalid UTC RTC value");
    struct tm utc = {};
    utc.tm_year = value.year - 1900;
    utc.tm_mon = value.month - 1;
    utc.tm_mday = value.day;
    utc.tm_hour = value.hour;
    utc.tm_min = value.minute;
    utc.tm_sec = value.second;
    *epoch = static_cast<time_t>(tm_as_utc_seconds(utc));
    return ESP_OK;
}

esp_err_t epoch_to_rtc(time_t epoch, RtcDateTime *value)
{
    ESP_RETURN_ON_FALSE(value != nullptr, ESP_ERR_INVALID_ARG, kTag, "missing RTC value");
    struct tm utc = {};
    ESP_RETURN_ON_FALSE(gmtime_r(&epoch, &utc) != nullptr,
                        ESP_FAIL,
                        kTag,
                        "failed to decode UTC time");
    RtcDateTime converted = {};
    converted.year = static_cast<uint16_t>(utc.tm_year + 1900);
    converted.month = static_cast<uint8_t>(utc.tm_mon + 1);
    converted.day = static_cast<uint8_t>(utc.tm_mday);
    converted.weekday = static_cast<uint8_t>(utc.tm_wday);
    converted.hour = static_cast<uint8_t>(utc.tm_hour);
    converted.minute = static_cast<uint8_t>(utc.tm_min);
    converted.second = static_cast<uint8_t>(utc.tm_sec);
    ESP_RETURN_ON_FALSE(rtc_datetime_valid(converted),
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "SNTP time is outside the RTC range");
    *value = converted;
    return ESP_OK;
}

esp_err_t set_system_time(time_t epoch, TimeSource source)
{
    const struct timeval now = {
        .tv_sec = epoch,
        .tv_usec = 0,
    };
    ESP_RETURN_ON_FALSE(settimeofday(&now, nullptr) == 0,
                        ESP_FAIL,
                        kTag,
                        "failed to set system time");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        kTag,
                        "time state is busy");
    g_clock_valid = true;
    g_source = source;
    xSemaphoreGive(g_mutex);
    return ESP_OK;
}

void sync_task_main(void *)
{
    while (true) {
        SyncMessage message = {};
        if (xQueueReceive(g_sync_queue, &message, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            g_clock_valid = true;
            g_source = TimeSource::Sntp;
            g_last_sntp_sync = message.seconds;
            xSemaphoreGive(g_mutex);
        }

        RtcStatus rtc_status = {};
        esp_err_t rtc_err = rtc_manager_get_status(&rtc_status);
        if (rtc_err == ESP_OK && !rtc_status.present) {
            rtc_err = ESP_ERR_NOT_SUPPORTED;
        } else if (rtc_err == ESP_OK && !rtc_status.initialized) {
            rtc_err = rtc_status.initialization_result;
        } else if (rtc_err == ESP_OK) {
            RtcDateTime value = {};
            rtc_err = epoch_to_rtc(message.seconds, &value);
            if (rtc_err == ESP_OK) {
                rtc_err = rtc_manager_set(value);
            }
        }

        struct tm utc = {};
        char text[24] = {};
        if (gmtime_r(&message.seconds, &utc) != nullptr) {
            strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &utc);
        } else {
            strlcpy(text, "unknown", sizeof(text));
        }
        if (rtc_err == ESP_OK) {
            ESP_LOGI(kTag, "SNTP synchronized UTC=%s; RTC updated", text);
        } else if (rtc_err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGI(kTag, "SNTP synchronized UTC=%s; RTC not present", text);
        } else {
            ESP_LOGW(kTag,
                     "SNTP synchronized UTC=%s; RTC update failed: %s",
                     text,
                     esp_err_to_name(rtc_err));
        }
    }
}

void sntp_sync_callback(struct timeval *value)
{
    if (value == nullptr || g_sync_queue == nullptr) {
        return;
    }
    const SyncMessage message = {
        .seconds = value->tv_sec,
    };
    xQueueOverwrite(g_sync_queue, &message);
}

}  // namespace

const char *time_source_name(TimeSource source)
{
    switch (source) {
    case TimeSource::Unset:
        return "unset";
    case TimeSource::Rtc:
        return "rtc";
    case TimeSource::Manual:
        return "manual";
    case TimeSource::Sntp:
        return "sntp";
    }
    return "unknown";
}

const char *time_manager_sntp_server(void)
{
    return kSntpServer;
}

size_t time_manager_timezone_count(void)
{
    return sizeof(kTimezones) / sizeof(kTimezones[0]);
}

const TimezoneInfo *time_manager_timezone_at(size_t index)
{
    return index < time_manager_timezone_count() ? &kTimezones[index] : nullptr;
}

esp_err_t time_manager_init(void)
{
    if (g_mutex != nullptr) {
        return ESP_OK;
    }
    g_mutex = xSemaphoreCreateMutexStatic(&g_mutex_storage);
    ESP_RETURN_ON_FALSE(g_mutex != nullptr,
                        ESP_ERR_NO_MEM,
                        kTag,
                        "failed to create time mutex");

    char stored[kTimezoneNameBytes] = {};
    esp_err_t err = load_timezone(stored, sizeof(stored));
    if (err != ESP_OK) {
        return err;
    }
    const TimezoneInfo *timezone = find_timezone(stored);
    if (timezone == nullptr) {
        ESP_LOGW(kTag, "stored timezone '%s' is unsupported; using UTC", stored);
        timezone = find_timezone(kDefaultTimezone);
    }
    return apply_timezone_locked(*timezone);
}

esp_err_t time_manager_seed_from_rtc(void)
{
    RtcDateTime value = {};
    ESP_RETURN_ON_ERROR(rtc_manager_read(&value), kTag, "failed to read RTC for system time");
    ESP_RETURN_ON_FALSE(!value.oscillator_stopped,
                        ESP_ERR_INVALID_STATE,
                        kTag,
                        "RTC oscillator-stop flag is set");

    time_t epoch = 0;
    ESP_RETURN_ON_ERROR(rtc_to_epoch(value, &epoch), kTag, "invalid RTC time");
    ESP_RETURN_ON_ERROR(set_system_time(epoch, TimeSource::Rtc),
                        kTag,
                        "failed to seed system time");
    ESP_LOGI(kTag,
             "system time seeded from RTC: %04u-%02u-%02u %02u:%02u:%02u UTC",
             value.year,
             value.month,
             value.day,
             value.hour,
             value.minute,
             value.second);
    return ESP_OK;
}

esp_err_t time_manager_start_sntp(void)
{
    if (g_sntp_started) {
        return ESP_OK;
    }
    if (g_sync_queue == nullptr) {
        g_sync_queue = xQueueCreateStatic(1,
                                          sizeof(SyncMessage),
                                          g_sync_queue_buffer,
                                          &g_sync_queue_storage);
        ESP_RETURN_ON_FALSE(g_sync_queue != nullptr,
                            ESP_ERR_NO_MEM,
                            kTag,
                            "failed to create SNTP update queue");
    }
    if (g_sync_task == nullptr) {
        const BaseType_t created = xTaskCreate(sync_task_main,
                                               "time_sync",
                                               kSyncTaskStackBytes,
                                               nullptr,
                                               kSyncTaskPriority,
                                               &g_sync_task);
        ESP_RETURN_ON_FALSE(created == pdPASS,
                            ESP_ERR_NO_MEM,
                            kTag,
                            "failed to create SNTP update task");
    }

    esp_sntp_config_t config = {};
    config.smooth_sync = false;
    config.server_from_dhcp = false;
    config.wait_for_sync = false;
    config.start = true;
    config.sync_cb = &sntp_sync_callback;
    config.num_of_servers = 1;
    config.servers[0] = kSntpServer;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&config),
                        kTag,
                        "failed to initialize SNTP");

    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        g_sntp_started = true;
        xSemaphoreGive(g_mutex);
    }
    ESP_LOGI(kTag, "SNTP started server=%s", kSntpServer);
    return ESP_OK;
}

esp_err_t time_manager_set_utc(const RtcDateTime &value)
{
    ESP_RETURN_ON_ERROR(rtc_manager_set(value), kTag, "failed to set RTC");
    time_t epoch = 0;
    ESP_RETURN_ON_ERROR(rtc_to_epoch(value, &epoch), kTag, "invalid UTC time");
    return set_system_time(epoch, TimeSource::Manual);
}

esp_err_t time_manager_set_timezone(const char *name)
{
    const TimezoneInfo *timezone = find_timezone(name);
    ESP_RETURN_ON_FALSE(timezone != nullptr,
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "unsupported timezone; use 'show timezones'");
    ESP_RETURN_ON_ERROR(save_timezone(timezone->human_name),
                        kTag,
                        "failed to save timezone");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        kTag,
                        "time state is busy");
    const esp_err_t err = apply_timezone_locked(*timezone);
    xSemaphoreGive(g_mutex);
    return err;
}

esp_err_t time_manager_get_status(TimeManagerStatus *status)
{
    ESP_RETURN_ON_FALSE(status != nullptr, ESP_ERR_INVALID_ARG, kTag, "missing status");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        kTag,
                        "time state is busy");
    TimeManagerStatus result = {};
    result.clock_valid = g_clock_valid;
    result.sntp_started = g_sntp_started;
    result.source = g_source;
    result.last_sntp_sync = g_last_sntp_sync;
    strlcpy(result.timezone_name, g_timezone_name, sizeof(result.timezone_name));
    strlcpy(result.posix_timezone, g_posix_timezone, sizeof(result.posix_timezone));
    if (g_clock_valid) {
        const time_t now = time(nullptr);
        struct tm local = {};
        if (localtime_r(&now, &local) != nullptr) {
            strftime(result.current_abbreviation,
                     sizeof(result.current_abbreviation),
                     "%Z",
                     &local);
        }
    }
    *status = result;
    xSemaphoreGive(g_mutex);
    return ESP_OK;
}

esp_err_t time_manager_get_time(TimeSnapshot *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != nullptr, ESP_ERR_INVALID_ARG, kTag, "missing time");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        kTag,
                        "time state is busy");

    TimeSnapshot result = {};
    result.valid = g_clock_valid;
    result.source = g_source;
    strlcpy(result.timezone_name, g_timezone_name, sizeof(result.timezone_name));
    if (g_clock_valid) {
        result.epoch = time(nullptr);
        if (gmtime_r(&result.epoch, &result.utc) == nullptr ||
            localtime_r(&result.epoch, &result.local) == nullptr) {
            xSemaphoreGive(g_mutex);
            return ESP_FAIL;
        }
        const int64_t local_as_utc = tm_as_utc_seconds(result.local);
        result.utc_offset_minutes =
            static_cast<int>((local_as_utc - static_cast<int64_t>(result.epoch)) / 60);
        strftime(result.abbreviation,
                 sizeof(result.abbreviation),
                 "%Z",
                 &result.local);
    }
    *snapshot = result;
    xSemaphoreGive(g_mutex);
    return ESP_OK;
}
