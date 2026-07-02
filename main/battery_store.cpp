#include "battery_store.hpp"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

namespace {

constexpr const char *kTag = "battery_store";

struct BatterySlot {
    bool occupied;
    BatteryRecord record;
};

BatterySlot g_batteries[CONFIG_POWER4_MAX_BATTERIES] = {};
SemaphoreHandle_t g_mutex = nullptr;

class StoreLock {
public:
    StoreLock(void)
    {
        if (g_mutex != nullptr) {
            locked_ = xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE;
        }
    }

    ~StoreLock(void)
    {
        if (locked_) {
            xSemaphoreGive(g_mutex);
        }
    }

    bool locked(void) const
    {
        return locked_;
    }

private:
    bool locked_ = false;
};

int find_by_name(const char *name)
{
    for (size_t i = 0; i < CONFIG_POWER4_MAX_BATTERIES; ++i) {
        if (g_batteries[i].occupied && strcmp(g_batteries[i].record.name, name) == 0) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

int find_empty(void)
{
    for (size_t i = 0; i < CONFIG_POWER4_MAX_BATTERIES; ++i) {
        if (!g_batteries[i].occupied) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

int find_least_recently_seen(void)
{
    size_t oldest = 0;
    for (size_t i = 1; i < CONFIG_POWER4_MAX_BATTERIES; ++i) {
        if (g_batteries[i].record.last_seen_us < g_batteries[oldest].record.last_seen_us) {
            oldest = i;
        }
    }

    return static_cast<int>(oldest);
}

void write_record(BatterySlot *slot,
                  const char *name,
                  float voltage_v,
                  float current_a,
                  float soc_percent,
                  int64_t now_us)
{
    strlcpy(slot->record.name, name, sizeof(slot->record.name));
    slot->record.voltage_v = voltage_v;
    slot->record.current_a = current_a;
    slot->record.soc_percent = soc_percent;
    slot->record.last_seen_us = now_us;
    slot->occupied = true;
}

}  // namespace

esp_err_t battery_store_init(void)
{
    if (g_mutex != nullptr) {
        return ESP_OK;
    }

    g_mutex = xSemaphoreCreateMutex();
    if (g_mutex == nullptr) {
        ESP_LOGE(kTag, "failed to create battery store mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(g_batteries, 0, sizeof(g_batteries));
    ESP_LOGI(kTag, "started with capacity for %u batteries", CONFIG_POWER4_MAX_BATTERIES);
    return ESP_OK;
}

bool battery_store_valid_name(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return false;
    }

    size_t length = 0;
    for (; name[length] != '\0'; ++length) {
        const unsigned char ch = static_cast<unsigned char>(name[length]);
        if (length >= kBatteryNameMax || ch <= ' ' || ch > '~') {
            return false;
        }
    }

    return length > 0;
}

size_t battery_store_capacity(void)
{
    return CONFIG_POWER4_MAX_BATTERIES;
}

esp_err_t battery_store_record_observation(const char *name,
                                           float voltage_v,
                                           float current_a,
                                           float soc_percent)
{
    if (!battery_store_valid_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    StoreLock lock;
    if (!lock.locked()) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t now_us = esp_timer_get_time();
    int slot_index = find_by_name(name);
    if (slot_index < 0) {
        slot_index = find_empty();
    }
    if (slot_index < 0) {
        slot_index = find_least_recently_seen();
        ESP_LOGW(kTag,
                 "evicting battery '%s' for new observation '%s'",
                 g_batteries[slot_index].record.name,
                 name);
    }

    write_record(&g_batteries[slot_index], name, voltage_v, current_a, soc_percent, now_us);
    return ESP_OK;
}

esp_err_t battery_store_get(const char *name, BatteryRecord *record)
{
    if (!battery_store_valid_name(name) || record == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    StoreLock lock;
    if (!lock.locked()) {
        return ESP_ERR_INVALID_STATE;
    }

    const int slot_index = find_by_name(name);
    if (slot_index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    *record = g_batteries[slot_index].record;
    return ESP_OK;
}

esp_err_t battery_store_list_names(BatteryNameList *names)
{
    if (names == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    StoreLock lock;
    if (!lock.locked()) {
        return ESP_ERR_INVALID_STATE;
    }

    names->count = 0;
    for (size_t i = 0; i < CONFIG_POWER4_MAX_BATTERIES; ++i) {
        if (!g_batteries[i].occupied) {
            continue;
        }

        strlcpy(names->names[names->count], g_batteries[i].record.name, sizeof(names->names[0]));
        ++names->count;
    }

    return ESP_OK;
}

esp_err_t battery_store_snapshot(BatteryRecord *records, size_t capacity, size_t *count)
{
    if (records == nullptr || count == nullptr || capacity < CONFIG_POWER4_MAX_BATTERIES) {
        return ESP_ERR_INVALID_ARG;
    }

    StoreLock lock;
    if (!lock.locked()) {
        return ESP_ERR_INVALID_STATE;
    }

    *count = 0;
    for (size_t i = 0; i < CONFIG_POWER4_MAX_BATTERIES; ++i) {
        if (!g_batteries[i].occupied) {
            continue;
        }

        records[*count] = g_batteries[i].record;
        ++(*count);
    }

    return ESP_OK;
}
