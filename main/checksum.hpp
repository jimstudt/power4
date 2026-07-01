#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

constexpr size_t kChecksumSha1Bytes = 20;
constexpr size_t kChecksumSha1HexChars = kChecksumSha1Bytes * 2;

esp_err_t checksum_sha1(const void *data, size_t length, uint8_t digest[kChecksumSha1Bytes]);
esp_err_t checksum_sha1_hex(const void *data,
                            size_t length,
                            char hex[kChecksumSha1HexChars + 1]);
void checksum_bytes_to_hex(const uint8_t *bytes, size_t byte_count, char *hex, size_t hex_capacity);
bool checksum_parse_sha1_hex(const char *text, uint8_t digest[kChecksumSha1Bytes]);
