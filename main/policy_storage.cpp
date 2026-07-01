#include "policy_storage.hpp"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr const char *kTag = "policy_storage";
constexpr const char *kNamespace = "policy";
constexpr const char *kActiveKey = "policy_active";
constexpr const char *kStagedKey = "policy_staged";

const char *key_for(PolicySlot slot)
{
    switch (slot) {
    case PolicySlot::Active:
        return kActiveKey;
    case PolicySlot::Staged:
        return kStagedKey;
    }

    return kActiveKey;
}

esp_err_t open_policy(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(kNamespace, mode, handle);
}

esp_err_t read_blob_alloc(nvs_handle_t handle, const char *key, char **contents, size_t *length)
{
    size_t stored_length = 0;
    esp_err_t err = nvs_get_blob(handle, key, nullptr, &stored_length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        stored_length = 0;
    } else if (err != ESP_OK) {
        return err;
    }

    char *buffer = static_cast<char *>(malloc(stored_length + 1));
    if (buffer == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    if (stored_length > 0) {
        err = nvs_get_blob(handle, key, buffer, &stored_length);
        if (err != ESP_OK) {
            free(buffer);
            return err;
        }
    }

    buffer[stored_length] = '\0';
    *contents = buffer;
    *length = stored_length;
    return ESP_OK;
}

}  // namespace

esp_err_t policy_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "failed to erase NVS");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, kTag, "failed to initialize NVS");

    nvs_handle_t handle = 0;
    err = open_policy(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_close(handle);
    return ESP_OK;
}

const char *policy_storage_slot_name(PolicySlot slot)
{
    switch (slot) {
    case PolicySlot::Active:
        return "active";
    case PolicySlot::Staged:
        return "staged";
    }

    return "active";
}

esp_err_t policy_storage_read_alloc(PolicySlot slot, char **contents, size_t *length)
{
    if (contents == nullptr || length == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *contents = nullptr;
    *length = 0;

    nvs_handle_t handle = 0;
    esp_err_t err = open_policy(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = read_blob_alloc(handle, key_for(slot), contents, length);
    nvs_close(handle);
    return err;
}

esp_err_t policy_storage_write_staged(const void *contents, size_t length)
{
    if (contents == nullptr && length > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = open_policy(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    if (length == 0) {
        err = nvs_erase_key(handle, kStagedKey);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_blob(handle, kStagedKey, contents, length);
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

esp_err_t policy_storage_accept_staged(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = open_policy(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    char *staged = nullptr;
    size_t staged_length = 0;
    err = read_blob_alloc(handle, kStagedKey, &staged, &staged_length);
    if (err == ESP_OK) {
        if (staged_length == 0) {
            err = nvs_erase_key(handle, kActiveKey);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        } else {
            err = nvs_set_blob(handle, kActiveKey, staged, staged_length);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    free(staged);
    nvs_close(handle);
    return err;
}

esp_err_t policy_storage_get_stats(nvs_stats_t *stats)
{
    if (stats == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    return nvs_get_stats(nullptr, stats);
}

esp_err_t policy_storage_get_partition_size(size_t *size_bytes)
{
    if (size_bytes == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, nullptr);
    if (partition == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    *size_bytes = partition->size;
    return ESP_OK;
}
