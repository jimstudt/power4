#include "battery_store.hpp"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "utils.hpp"

namespace {

constexpr const char *kTag = "battery_store";

struct BatterySlot {
    bool occupied;
    BatteryRecord record;
};

BatterySlot g_batteries[CONFIG_POWER4_MAX_BATTERIES] = {};
SemaphoreHandle_t g_mutex = nullptr;

int find_by_name(const char *name)
{
    for (int i = 0; i < CONFIG_POWER4_MAX_BATTERIES; ++i) {
        if (g_batteries[i].occupied && strcmp(g_batteries[i].record.name, name) == 0) {
            return i;
        }
    }

    return -1;
}

int find_empty(void)
{
    for (int i = 0; i < CONFIG_POWER4_MAX_BATTERIES; ++i) {
        if (!g_batteries[i].occupied) {
            return i;
        }
    }

    return -1;
}

int find_least_recently_seen(void)
{
    int oldest = 0;
    for (int i = 1; i < CONFIG_POWER4_MAX_BATTERIES; ++i) {
        if (g_batteries[i].record.last_seen_us < g_batteries[oldest].record.last_seen_us) {
            oldest = i;
        }
    }

    return oldest;
}

void write_record(BatterySlot *slot,
                  const char *name,
                  const BatteryObservation &observation,
                  int64_t now_us)
{
    const bool preserve_cells =
        slot->occupied && slot->record.cell_voltages_valid &&
        slot->record.reported_cell_count == observation.reported_cell_count;

    strlcpy(slot->record.name, name, sizeof(slot->record.name));
    slot->record.voltage_v = observation.voltage_v;
    slot->record.current_a = observation.current_a;
    slot->record.soc_percent = observation.soc_percent;
    slot->record.cycle_count = observation.cycle_count;
    slot->record.protection_status = observation.protection_status;
    slot->record.reported_cell_count = observation.reported_cell_count;
    slot->record.temperature_valid = observation.temperature_valid;
    slot->record.temperature_c = observation.temperature_c;

    if (observation.cell_voltages_valid) {
        slot->record.cell_voltages_valid = true;
        slot->record.cell_voltage_count = observation.cell_voltage_count;
        memcpy(slot->record.cell_voltages_mv,
               observation.cell_voltages_mv,
               observation.cell_voltage_count * sizeof(observation.cell_voltages_mv[0]));
        slot->record.cell_last_seen_us = now_us;
    } else if (!preserve_cells) {
        slot->record.cell_voltages_valid = false;
        slot->record.cell_voltage_count = 0;
        memset(slot->record.cell_voltages_mv, 0, sizeof(slot->record.cell_voltages_mv));
        slot->record.cell_last_seen_us = 0;
    }

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

esp_err_t battery_store_record_observation(const char *name, const BatteryObservation *observation)
{
    if (!battery_store_valid_name(name) || observation == nullptr ||
        (observation->cell_voltages_valid &&
         (observation->cell_voltage_count == 0 ||
          observation->cell_voltage_count > kBatteryCellMax ||
          observation->cell_voltage_count != observation->reported_cell_count))) {
        return ESP_ERR_INVALID_ARG;
    }

    ScopedLock lock(g_mutex);
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

    write_record(&g_batteries[slot_index], name, *observation, now_us);
    return ESP_OK;
}

esp_err_t battery_store_get(const char *name, BatteryRecord *record)
{
    if (!battery_store_valid_name(name) || record == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    ScopedLock lock(g_mutex);
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

    ScopedLock lock(g_mutex);
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

    ScopedLock lock(g_mutex);
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

bool battery_record_cell_summary(const BatteryRecord *record, BatteryCellSummary *summary)
{
    if (record == nullptr || summary == nullptr || !record->cell_voltages_valid ||
        record->cell_voltage_count == 0 || record->cell_voltage_count > kBatteryCellMax) {
        return false;
    }

    BatteryCellSummary computed = {
        .min_voltage_v = record->cell_voltages_mv[0] / 1000.0f,
        .max_voltage_v = record->cell_voltages_mv[0] / 1000.0f,
        .delta_voltage_v = 0.0f,
        .min_cell_number = 1,
        .max_cell_number = 1,
    };
    for (uint8_t i = 1; i < record->cell_voltage_count; ++i) {
        const float voltage_v = record->cell_voltages_mv[i] / 1000.0f;
        if (voltage_v < computed.min_voltage_v) {
            computed.min_voltage_v = voltage_v;
            computed.min_cell_number = i + 1;
        }
        if (voltage_v > computed.max_voltage_v) {
            computed.max_voltage_v = voltage_v;
            computed.max_cell_number = i + 1;
        }
    }
    computed.delta_voltage_v = computed.max_voltage_v - computed.min_voltage_v;
    *summary = computed;
    return true;
}

uint32_t battery_record_cell_age_s(const BatteryRecord *record, int64_t now_us)
{
    if (record == nullptr || !record->cell_voltages_valid || record->cell_last_seen_us <= 0 ||
        now_us <= record->cell_last_seen_us) {
        return 0;
    }

    const int64_t age_s = (now_us - record->cell_last_seen_us) / 1000000LL;
    return age_s > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(age_s);
}
