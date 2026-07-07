#pragma once

#include <stddef.h>

#include "esp_err.h"

esp_err_t policy_task_start(void);
// Compile-check a policy program without running it. Returns ESP_OK when the
// source parses; otherwise copies the Lua error into error_message (which may
// be null) and returns ESP_FAIL.
esp_err_t policy_validate(const char *source,
                          size_t length,
                          char *error_message,
                          size_t error_message_size);
