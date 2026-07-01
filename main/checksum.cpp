#include "checksum.hpp"

#include <string.h>

#include "esp_err.h"
#include "psa/crypto.h"

namespace {

int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

} // namespace

esp_err_t checksum_sha1(const void *data, size_t length, uint8_t digest[kChecksumSha1Bytes])
{
    if (digest == nullptr || (data == nullptr && length > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t empty = 0;
    const unsigned char *input =
        data == nullptr ? &empty : static_cast<const unsigned char *>(data);

    size_t digest_length = 0;
    const psa_status_t status = psa_hash_compute(PSA_ALG_SHA_1,
                                                 input,
                                                 length,
                                                 digest,
                                                 kChecksumSha1Bytes,
                                                 &digest_length);
    if (status != PSA_SUCCESS || digest_length != kChecksumSha1Bytes) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

void checksum_bytes_to_hex(const uint8_t *bytes, size_t byte_count, char *hex, size_t hex_capacity)
{
    static constexpr char kHex[] = "0123456789abcdef";

    if (bytes == nullptr || hex == nullptr || hex_capacity == 0) {
        return;
    }

    const size_t max_bytes = (hex_capacity - 1) / 2;
    const size_t bytes_to_write = byte_count < max_bytes ? byte_count : max_bytes;
    for (size_t i = 0; i < bytes_to_write; ++i) {
        hex[i * 2] = kHex[(bytes[i] >> 4) & 0x0f];
        hex[(i * 2) + 1] = kHex[bytes[i] & 0x0f];
    }
    hex[bytes_to_write * 2] = '\0';
}

esp_err_t checksum_sha1_hex(const void *data,
                            size_t length,
                            char hex[kChecksumSha1HexChars + 1])
{
    if (hex == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t digest[kChecksumSha1Bytes] = {};
    const esp_err_t err = checksum_sha1(data, length, digest);
    if (err != ESP_OK) {
        return err;
    }

    checksum_bytes_to_hex(digest, sizeof(digest), hex, kChecksumSha1HexChars + 1);
    return ESP_OK;
}

bool checksum_parse_sha1_hex(const char *text, uint8_t digest[kChecksumSha1Bytes])
{
    if (text == nullptr || digest == nullptr || strlen(text) != kChecksumSha1HexChars) {
        return false;
    }

    for (size_t i = 0; i < kChecksumSha1Bytes; ++i) {
        const int high = hex_value(text[i * 2]);
        const int low = hex_value(text[(i * 2) + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        digest[i] = static_cast<uint8_t>((high << 4) | low);
    }

    return true;
}
