#pragma once

#include <stddef.h>

#include "esp_err.h"

constexpr uint16_t kNetworkConsolePort = 4244;
constexpr size_t kNetworkConsolePasswordMinBytes = 16;
constexpr size_t kNetworkConsolePasswordMaxBytes = 128;

esp_err_t network_console_init(void);
esp_err_t network_console_start(void);
esp_err_t network_console_password_set(const char *password);
esp_err_t network_console_password_generate(char *password, size_t capacity);
esp_err_t network_console_password_get(char *password, size_t capacity, bool *configured);
