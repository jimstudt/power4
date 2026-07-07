#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h"

constexpr size_t kConfigFlagNameMaxBytes = NVS_KEY_NAME_MAX_SIZE;
constexpr size_t kConfigFlagListMax = 32;

struct ConfigFlagList {
    size_t count = 0;
    bool truncated = false;
    char names[kConfigFlagListMax][kConfigFlagNameMaxBytes] = {};
    uint32_t lifetime_s[kConfigFlagListMax] = {};   // 0 when the flag is permanent
    uint32_t remaining_s[kConfigFlagListMax] = {};  // meaningful only when lifetime_s > 0
};

bool config_flags_valid_name(const char *name);
// lifetime_s == 0 sets a permanent flag; otherwise the flag is removed by
// config_flags_expire() once lifetime_s seconds pass without a refresh.
// After a reboot the countdown restarts from the full lifetime.
esp_err_t config_flags_set(const char *name, uint32_t lifetime_s);
esp_err_t config_flags_unset(const char *name);
esp_err_t config_flags_is_set(const char *name, bool *is_set);
esp_err_t config_flags_list(ConfigFlagList *list);
// Remove flags whose lifetime has elapsed. Called before each policy cycle.
esp_err_t config_flags_expire(void);
