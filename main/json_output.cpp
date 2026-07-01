#include "json_output.hpp"

#include <stdio.h>
#include <string.h>

#include "checksum.hpp"
#include "esp_err.h"

esp_err_t json_output_print(const char *json)
{
    if (json == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t length = strlen(json);
    char sha1_hex[kChecksumSha1HexChars + 1] = {};
    const esp_err_t err = checksum_sha1_hex(json, length, sha1_hex);
    if (err != ESP_OK) {
        return err;
    }

    printf("P4J1 %u %s %s\n", static_cast<unsigned>(length), sha1_hex, json);
    return ESP_OK;
}
