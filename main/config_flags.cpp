#include "config_flags.hpp"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"

namespace {

constexpr const char *kTag = "config_flags";
constexpr const char *kNamespace = "config";
// Lifetimes live in their own namespace so the flag entries in "config"
// stay plain u8 values. Key names match the flag names.
constexpr const char *kTtlNamespace = "policy_ttl";

// Uptime-based deadlines for flags with a lifetime. Entries are rebuilt on
// demand after a reboot, restarting each countdown from its full lifetime.
struct FlagDeadline {
    bool used = false;
    char name[kConfigFlagNameMaxBytes] = {};
    uint32_t lifetime_s = 0;
    int64_t deadline_us = 0;
};

FlagDeadline g_deadlines[kConfigFlagListMax] = {};
portMUX_TYPE g_deadline_lock = portMUX_INITIALIZER_UNLOCKED;

bool is_name_char(char c)
{
    return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}

esp_err_t validate_name(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t length = strlen(name);
    if (length >= kConfigFlagNameMaxBytes) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < length; ++i) {
        if (!is_name_char(name[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

esp_err_t open_config(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(kNamespace, mode, handle);
}

esp_err_t open_ttl(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(kTtlNamespace, mode, handle);
}

void deadline_drop(const char *name)
{
    taskENTER_CRITICAL(&g_deadline_lock);
    for (size_t i = 0; i < kConfigFlagListMax; ++i) {
        if (g_deadlines[i].used && strcmp(g_deadlines[i].name, name) == 0) {
            g_deadlines[i] = FlagDeadline {};
            break;
        }
    }
    taskEXIT_CRITICAL(&g_deadline_lock);
}

// Set or refresh the countdown for a flag. Returns false if the table is
// full, which callers treat as a hard error rather than a silent leak.
bool deadline_refresh(const char *name, uint32_t lifetime_s)
{
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(lifetime_s) * 1000000;

    bool stored = false;
    taskENTER_CRITICAL(&g_deadline_lock);
    FlagDeadline *slot = nullptr;
    for (size_t i = 0; i < kConfigFlagListMax; ++i) {
        if (g_deadlines[i].used && strcmp(g_deadlines[i].name, name) == 0) {
            slot = &g_deadlines[i];
            break;
        }
        if (slot == nullptr && !g_deadlines[i].used) {
            slot = &g_deadlines[i];
        }
    }
    if (slot != nullptr) {
        slot->used = true;
        strlcpy(slot->name, name, sizeof(slot->name));
        slot->lifetime_s = lifetime_s;
        slot->deadline_us = deadline_us;
        stored = true;
    }
    taskEXIT_CRITICAL(&g_deadline_lock);

    return stored;
}

// Look up the countdown for a flag, creating it (full lifetime) if this is
// the first sighting since boot. Returns false if no entry could be made.
bool deadline_query(const char *name, uint32_t lifetime_s, uint32_t *remaining_s)
{
    *remaining_s = 0;

    const int64_t now_us = esp_timer_get_time();

    bool found = false;
    taskENTER_CRITICAL(&g_deadline_lock);
    for (size_t i = 0; i < kConfigFlagListMax; ++i) {
        if (g_deadlines[i].used && strcmp(g_deadlines[i].name, name) == 0) {
            const int64_t left_us = g_deadlines[i].deadline_us - now_us;
            if (left_us > 0) {
                *remaining_s = static_cast<uint32_t>((left_us + 999999) / 1000000);
            }
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&g_deadline_lock);

    if (found) {
        return true;
    }

    if (!deadline_refresh(name, lifetime_s)) {
        return false;
    }
    *remaining_s = lifetime_s;
    return true;
}

esp_err_t ttl_erase(const char *name)
{
    nvs_handle_t handle = 0;
    esp_err_t err = open_ttl(NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, name);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

esp_err_t ttl_get(const char *name, uint32_t *lifetime_s)
{
    *lifetime_s = 0;

    nvs_handle_t handle = 0;
    esp_err_t err = open_ttl(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u32(handle, name, lifetime_s);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *lifetime_s = 0;
        return ESP_OK;
    }
    return err;
}

}  // namespace

bool config_flags_valid_name(const char *name)
{
    return validate_name(name) == ESP_OK;
}

esp_err_t config_flags_set(const char *name, uint32_t lifetime_s)
{
    esp_err_t err = validate_name(name);
    if (err != ESP_OK) {
        return err;
    }

    // Record the lifetime before the flag itself so an interruption cannot
    // leave a lifetime flag behind as a permanent one. An orphaned lifetime
    // entry is cleaned up by config_flags_expire().
    if (lifetime_s > 0) {
        if (!deadline_refresh(name, lifetime_s)) {
            return ESP_ERR_NO_MEM;
        }

        nvs_handle_t ttl_handle = 0;
        err = open_ttl(NVS_READWRITE, &ttl_handle);
        if (err != ESP_OK) {
            deadline_drop(name);
            return err;
        }
        err = nvs_set_u32(ttl_handle, name, lifetime_s);
        if (err == ESP_OK) {
            err = nvs_commit(ttl_handle);
        }
        nvs_close(ttl_handle);
        if (err != ESP_OK) {
            deadline_drop(name);
            return err;
        }
    } else {
        err = ttl_erase(name);
        if (err != ESP_OK) {
            return err;
        }
        deadline_drop(name);
    }

    nvs_handle_t handle = 0;
    err = open_config(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, name, 1);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

esp_err_t config_flags_unset(const char *name)
{
    esp_err_t err = validate_name(name);
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle = 0;
    err = open_config(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, name);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        err = ttl_erase(name);
        deadline_drop(name);
    }
    return err;
}

esp_err_t config_flags_is_set(const char *name, bool *is_set)
{
    if (is_set == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *is_set = false;

    esp_err_t err = validate_name(name);
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle = 0;
    err = open_config(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t value = 0;
    err = nvs_get_u8(handle, name, &value);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    *is_set = value != 0;
    return ESP_OK;
}

esp_err_t config_flags_list(ConfigFlagList *list)
{
    if (list == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *list = ConfigFlagList {};

    nvs_iterator_t iterator = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, kNamespace, NVS_TYPE_U8, &iterator);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    while (iterator != nullptr) {
        nvs_entry_info_t info = {};
        err = nvs_entry_info(iterator, &info);
        if (err != ESP_OK) {
            nvs_release_iterator(iterator);
            return err;
        }

        if (list->count < kConfigFlagListMax) {
            strlcpy(list->names[list->count], info.key, kConfigFlagNameMaxBytes);

            uint32_t lifetime_s = 0;
            if (ttl_get(info.key, &lifetime_s) == ESP_OK && lifetime_s > 0) {
                uint32_t remaining_s = 0;
                deadline_query(info.key, lifetime_s, &remaining_s);
                list->lifetime_s[list->count] = lifetime_s;
                list->remaining_s[list->count] = remaining_s;
            }

            ++list->count;
        } else {
            list->truncated = true;
        }

        err = nvs_entry_next(&iterator);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return ESP_OK;
        }
        if (err != ESP_OK) {
            nvs_release_iterator(iterator);
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t config_flags_expire(void)
{
    // Collect the lifetime entries first; expiry work below reopens NVS and
    // must not run while an iterator holds it.
    struct TtlEntry {
        char name[kConfigFlagNameMaxBytes];
        uint32_t lifetime_s;
    };
    TtlEntry entries[kConfigFlagListMax] = {};
    size_t entry_count = 0;

    nvs_iterator_t iterator = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, kTtlNamespace, NVS_TYPE_U32, &iterator);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    while (iterator != nullptr && entry_count < kConfigFlagListMax) {
        nvs_entry_info_t info = {};
        err = nvs_entry_info(iterator, &info);
        if (err != ESP_OK) {
            nvs_release_iterator(iterator);
            return err;
        }

        strlcpy(entries[entry_count].name, info.key, kConfigFlagNameMaxBytes);
        if (ttl_get(info.key, &entries[entry_count].lifetime_s) == ESP_OK) {
            ++entry_count;
        }

        err = nvs_entry_next(&iterator);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            iterator = nullptr;
            break;
        }
        if (err != ESP_OK) {
            nvs_release_iterator(iterator);
            return err;
        }
    }
    nvs_release_iterator(iterator);

    for (size_t i = 0; i < entry_count; ++i) {
        const char *name = entries[i].name;

        bool is_set = false;
        if (config_flags_is_set(name, &is_set) != ESP_OK) {
            continue;
        }
        if (!is_set) {
            // Orphaned lifetime entry, e.g. from an interrupted set.
            ttl_erase(name);
            deadline_drop(name);
            continue;
        }

        uint32_t remaining_s = 0;
        if (!deadline_query(name, entries[i].lifetime_s, &remaining_s)) {
            continue;
        }
        if (remaining_s == 0) {
            ESP_LOGI(kTag, "policy flag %s expired after %us", name, entries[i].lifetime_s);
            config_flags_unset(name);
        }
    }

    return ESP_OK;
}
