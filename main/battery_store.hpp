#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

constexpr size_t kBatteryNameMax = 31;
constexpr size_t kBatteryCellMax = 32;

struct BatteryObservation {
    float voltage_v;
    float current_a;
    float soc_percent;
    uint16_t cycle_count;
    uint16_t protection_status;
    uint8_t reported_cell_count;
    bool temperature_valid;
    float temperature_c;
    bool cell_voltages_valid;
    uint8_t cell_voltage_count;
    uint16_t cell_voltages_mv[kBatteryCellMax];
};

// Battery names are 1-31 printable non-space ASCII characters.
struct BatteryRecord {
    char name[kBatteryNameMax + 1];
    float voltage_v;
    float current_a;
    float soc_percent;
    uint16_t cycle_count;
    uint16_t protection_status;
    uint8_t reported_cell_count;
    bool temperature_valid;
    float temperature_c;
    bool cell_voltages_valid;
    uint8_t cell_voltage_count;
    uint16_t cell_voltages_mv[kBatteryCellMax];
    int64_t cell_last_seen_us;
    int64_t last_seen_us;
};

struct BatteryCellSummary {
    float min_voltage_v;
    float max_voltage_v;
    float delta_voltage_v;
    uint8_t min_cell_number;
    uint8_t max_cell_number;
};

struct BatteryNameList {
    size_t count;
    char names[CONFIG_POWER4_MAX_BATTERIES][kBatteryNameMax + 1];
};

esp_err_t battery_store_init(void);
bool battery_store_valid_name(const char *name);
size_t battery_store_capacity(void);
esp_err_t battery_store_record_observation(const char *name, const BatteryObservation *observation);
esp_err_t battery_store_get(const char *name, BatteryRecord *record);
esp_err_t battery_store_list_names(BatteryNameList *names);
esp_err_t battery_store_snapshot(BatteryRecord *records, size_t capacity, size_t *count);
bool battery_record_cell_summary(const BatteryRecord *record, BatteryCellSummary *summary);
uint32_t battery_record_cell_age_s(const BatteryRecord *record, int64_t now_us);
