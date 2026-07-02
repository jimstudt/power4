#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

constexpr size_t kBatteryNameMax = 31;

// Battery names are 1-31 printable non-space ASCII characters.
struct BatteryRecord {
    char name[kBatteryNameMax + 1];
    float voltage_v;
    float current_a;
    float soc_percent;
    int64_t last_seen_us;
};

struct BatteryNameList {
    size_t count;
    char names[CONFIG_POWER4_MAX_BATTERIES][kBatteryNameMax + 1];
};

esp_err_t battery_store_init(void);
bool battery_store_valid_name(const char *name);
size_t battery_store_capacity(void);
esp_err_t battery_store_record_observation(const char *name,
                                           float voltage_v,
                                           float current_a,
                                           float soc_percent);
esp_err_t battery_store_get(const char *name, BatteryRecord *record);
esp_err_t battery_store_list_names(BatteryNameList *names);
esp_err_t battery_store_snapshot(BatteryRecord *records, size_t capacity, size_t *count);
