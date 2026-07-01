#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

uint32_t json_output_crc32(const char *json, size_t length);
esp_err_t json_output_print(const char *json);
