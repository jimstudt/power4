#pragma once

#include <stddef.h>

#include "battery_store.hpp"
#include "esp_err.h"

constexpr size_t kBatteryBankNameMax = 31;
constexpr size_t kBatteryBankMaxBanks = CONFIG_POWER4_MAX_BANKS;

struct BatteryBankDefinition {
    char name[kBatteryBankNameMax + 1];
    size_t battery_count;
    char batteries[CONFIG_POWER4_MAX_BATTERIES][kBatteryNameMax + 1];
};

struct BatteryBankList {
    size_t count;
    BatteryBankDefinition banks[kBatteryBankMaxBanks];
};

struct BatteryBankState {
    bool ready;
    float voltage_v;
    float current_a;
    float soc_percent;
};

esp_err_t battery_bank_init(void);
bool battery_bank_valid_name(const char *name);
esp_err_t battery_bank_create(const char *name, const char *const *battery_names, size_t battery_count);
esp_err_t battery_bank_remove(const char *name);
esp_err_t battery_bank_list(BatteryBankList *list);
esp_err_t battery_bank_get_state(const char *name, BatteryBankState *state);
