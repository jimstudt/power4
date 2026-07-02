#include "config_flags.hpp"

#include <ctype.h>
#include <string.h>

#include "nvs_flash.h"

namespace {

constexpr const char *kNamespace = "config";

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

}  // namespace

bool config_flags_valid_name(const char *name)
{
    return validate_name(name) == ESP_OK;
}

esp_err_t config_flags_set(const char *name)
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
