#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "nvs.h"

constexpr size_t kConfigFlagNameMaxBytes = NVS_KEY_NAME_MAX_SIZE;
constexpr size_t kConfigFlagListMax = 32;

struct ConfigFlagList {
    size_t count = 0;
    bool truncated = false;
    char names[kConfigFlagListMax][kConfigFlagNameMaxBytes] = {};
};

bool config_flags_valid_name(const char *name);
esp_err_t config_flags_set(const char *name);
esp_err_t config_flags_unset(const char *name);
esp_err_t config_flags_is_set(const char *name, bool *is_set);
esp_err_t config_flags_list(ConfigFlagList *list);
