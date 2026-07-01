#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "nvs_flash.h"

enum class PolicySlot {
    Active,
    Staged,
};

esp_err_t policy_storage_init(void);
const char *policy_storage_slot_name(PolicySlot slot);
esp_err_t policy_storage_read_alloc(PolicySlot slot, char **contents, size_t *length);
esp_err_t policy_storage_write_staged(const void *contents, size_t length);
esp_err_t policy_storage_accept_staged(void);
esp_err_t policy_storage_get_stats(nvs_stats_t *stats);
esp_err_t policy_storage_get_partition_size(size_t *size_bytes);
