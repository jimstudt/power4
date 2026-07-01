#include "json_output.hpp"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_rom_crc.h"

uint32_t json_output_crc32(const char *json, size_t length)
{
    return ~esp_rom_crc32_le(~0U, reinterpret_cast<const uint8_t *>(json), length);
}

esp_err_t json_output_print(const char *json)
{
    if (json == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t length = strlen(json);
    const uint32_t crc = json_output_crc32(json, length);
    printf("P4J1 %u %08" PRIX32 " %s\n", static_cast<unsigned>(length), crc, json);
    return ESP_OK;
}
