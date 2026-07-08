#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h"

constexpr size_t kConfigFlagNameMaxBytes = NVS_KEY_NAME_MAX_SIZE;
constexpr size_t kConfigFlagListMax = 32;
// Numeric values are stored as their validated text, so they stay
// inspectable in NVS and keep the author's integer/float distinction.
constexpr size_t kConfigNumberTextMaxBytes = 32;

// A policy name holds one value, stored as an NVS string: "true" or
// "false" for booleans, numeric text for numbers. Setting one kind
// replaces the other.
enum class ConfigFlagType : uint8_t {
    NotSet,
    Boolean,
    Number,
};

struct ConfigNumber {
    bool is_integer = false;
    long long integer = 0;  // valid when is_integer
    double value = 0.0;     // always valid
};

struct ConfigFlagList {
    size_t count = 0;
    bool truncated = false;
    char names[kConfigFlagListMax][kConfigFlagNameMaxBytes] = {};
    // Value rendered as text: numeric text for numbers, "true"/"false"
    // for booleans. Empty only if the value could not be read.
    char values[kConfigFlagListMax][kConfigNumberTextMaxBytes] = {};
    uint32_t lifetime_s[kConfigFlagListMax] = {};   // 0 when the flag is permanent
    uint32_t remaining_s[kConfigFlagListMax] = {};  // meaningful only when lifetime_s > 0
};

bool config_flags_valid_name(const char *name);
// Validate and decode numeric text: decimal integers or finite floats,
// e.g. "40", "-3", "39.5", "1e3". Rejects hex, inf, and nan.
bool config_number_parse(const char *text, ConfigNumber *number);
// Store an explicit boolean. False is stored, not erased, so a policy
// program can distinguish "set false" from "never defined" and apply a
// default; config_flags_unset() returns the name to undefined.
// lifetime_s == 0 sets a permanent value; otherwise it is removed by
// config_flags_expire() once lifetime_s seconds pass without a refresh.
// After a reboot the countdown restarts from the full lifetime.
esp_err_t config_flags_set(const char *name, bool value, uint32_t lifetime_s);
// Store a numeric value; text must satisfy config_number_parse(). The
// lifetime works the same as for boolean flags.
esp_err_t config_flags_set_number(const char *name, const char *text, uint32_t lifetime_s);
esp_err_t config_flags_unset(const char *name);
// True only when the name holds an explicit true boolean.
esp_err_t config_flags_is_set(const char *name, bool *is_set);
// found is false when the name is unset or holds a number.
esp_err_t config_flags_get_bool(const char *name, bool *value, bool *found);
// found is false when the name is unset or holds a boolean flag.
esp_err_t config_flags_get_number(const char *name, char *text, size_t text_bytes, bool *found);
esp_err_t config_flags_type(const char *name, ConfigFlagType *type);
esp_err_t config_flags_list(ConfigFlagList *list);
// Remove flags whose lifetime has elapsed. Called before each policy cycle.
esp_err_t config_flags_expire(void);
