#include "espnow_protocol.hpp"

#include <stdio.h>
#include <string.h>

namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool base64_encode(const uint8_t *input,
                   size_t input_length,
                   char *output,
                   size_t output_capacity,
                   size_t *output_length)
{
    const size_t needed = ((input_length + 2) / 3) * 4;
    if ((input == nullptr && input_length != 0) || output == nullptr ||
        output_length == nullptr || output_capacity < needed) {
        return false;
    }

    size_t source = 0;
    size_t destination = 0;
    while (source + 3 <= input_length) {
        const uint32_t value = (static_cast<uint32_t>(input[source]) << 16) |
                               (static_cast<uint32_t>(input[source + 1]) << 8) |
                               input[source + 2];
        output[destination++] = kBase64Alphabet[(value >> 18) & 0x3f];
        output[destination++] = kBase64Alphabet[(value >> 12) & 0x3f];
        output[destination++] = kBase64Alphabet[(value >> 6) & 0x3f];
        output[destination++] = kBase64Alphabet[value & 0x3f];
        source += 3;
    }

    const size_t remaining = input_length - source;
    if (remaining == 1) {
        const uint32_t value = static_cast<uint32_t>(input[source]) << 16;
        output[destination++] = kBase64Alphabet[(value >> 18) & 0x3f];
        output[destination++] = kBase64Alphabet[(value >> 12) & 0x3f];
        output[destination++] = '=';
        output[destination++] = '=';
    } else if (remaining == 2) {
        const uint32_t value = (static_cast<uint32_t>(input[source]) << 16) |
                               (static_cast<uint32_t>(input[source + 1]) << 8);
        output[destination++] = kBase64Alphabet[(value >> 18) & 0x3f];
        output[destination++] = kBase64Alphabet[(value >> 12) & 0x3f];
        output[destination++] = kBase64Alphabet[(value >> 6) & 0x3f];
        output[destination++] = '=';
    }

    *output_length = destination;
    return destination == needed;
}

int hex_digit(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

}  // namespace

bool espnow_protocol_valid_name(const char *name)
{
    if (name == nullptr) {
        return false;
    }
    const size_t length = strlen(name);
    if (length == 0 || length > 31) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const char ch = name[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

bool espnow_protocol_parse_mac(const char *text, uint8_t mac[6])
{
    if (text == nullptr || mac == nullptr || strlen(text) != 17) {
        return false;
    }
    for (size_t i = 0; i < 6; ++i) {
        const size_t offset = i * 3;
        const int high = hex_digit(text[offset]);
        const int low = hex_digit(text[offset + 1]);
        if (high < 0 || low < 0 || (i < 5 && text[offset + 2] != ':')) {
            return false;
        }
        mac[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

void espnow_protocol_format_mac(const uint8_t mac[6], char output[18])
{
    snprintf(output,
             18,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
}

size_t espnow_protocol_fragment_count(size_t content_length)
{
    return content_length == 0
               ? 1
               : (content_length + kEspNowProtocolReportChunkBytes - 1) /
                     kEspNowProtocolReportChunkBytes;
}

bool espnow_protocol_build_report_frame(const char *name,
                                        const char *frame_type,
                                        uint32_t message_id,
                                        size_t fragment_index,
                                        size_t fragment_count,
                                        size_t content_length,
                                        const uint8_t *fragment,
                                        size_t fragment_length,
                                        char *output,
                                        size_t output_capacity,
                                        size_t *output_length)
{
    if (!espnow_protocol_valid_name(name) || frame_type == nullptr || output == nullptr ||
        output_length == nullptr || fragment_index >= fragment_count ||
        fragment_length > kEspNowProtocolReportChunkBytes ||
        (fragment == nullptr && fragment_length != 0)) {
        return false;
    }

    const int prefix = snprintf(output,
                                output_capacity,
                                "{\"protocol\":\"power4-espnow\",\"version\":1,"
                                "\"name\":\"%s\",\"frame_type\":\"%s\","
                                "\"message_id\":%lu,\"fragment\":%u,\"fragments\":%u,"
                                "\"content_length\":%u,\"content_encoding\":\"base64\","
                                "\"content\":\"",
                                name,
                                frame_type,
                                static_cast<unsigned long>(message_id),
                                static_cast<unsigned>(fragment_index + 1),
                                static_cast<unsigned>(fragment_count),
                                static_cast<unsigned>(content_length));
    if (prefix < 0 || static_cast<size_t>(prefix) >= output_capacity) {
        return false;
    }

    size_t encoded = 0;
    if (!base64_encode(fragment,
                       fragment_length,
                       output + prefix,
                       output_capacity - static_cast<size_t>(prefix),
                       &encoded)) {
        return false;
    }
    const size_t used = static_cast<size_t>(prefix) + encoded;
    if (used + 3 > output_capacity) {
        return false;
    }
    output[used] = '"';
    output[used + 1] = '}';
    output[used + 2] = '\0';
    *output_length = used + 2;
    return *output_length <= kEspNowProtocolFrameMaxBytes;
}

bool espnow_protocol_build_gateway_frame(const EspNowRadioMetadata &metadata,
                                         const uint8_t *payload,
                                         size_t payload_length,
                                         char *output,
                                         size_t output_capacity,
                                         size_t *output_length)
{
    if (payload_length > kEspNowProtocolMaxPayloadBytes ||
        (payload == nullptr && payload_length != 0) || output == nullptr ||
        output_length == nullptr) {
        return false;
    }

    char source[18] = {};
    char destination[18] = {};
    espnow_protocol_format_mac(metadata.source_mac, source);
    espnow_protocol_format_mac(metadata.destination_mac, destination);
    const int prefix = snprintf(output,
                                output_capacity,
                                "{\"protocol\":\"power4-espnow-gateway\",\"version\":1,"
                                "\"source_mac\":\"%s\",\"destination_mac\":\"%s\","
                                "\"rssi_dbm\":%d,\"channel\":%u,\"rate\":%u,"
                                "\"signal_mode\":%u,\"mcs\":%u,\"noise_floor_dbm\":%d,"
                                "\"size\":%u,\"payload_encoding\":\"base64\",\"content\":\"",
                                source,
                                destination,
                                metadata.rssi_dbm,
                                metadata.channel,
                                metadata.rate,
                                metadata.signal_mode,
                                metadata.mcs,
                                metadata.noise_floor_dbm,
                                static_cast<unsigned>(payload_length));
    if (prefix < 0 || static_cast<size_t>(prefix) >= output_capacity) {
        return false;
    }

    size_t encoded = 0;
    if (!base64_encode(payload,
                       payload_length,
                       output + prefix,
                       output_capacity - static_cast<size_t>(prefix),
                       &encoded)) {
        return false;
    }
    const size_t used = static_cast<size_t>(prefix) + encoded;
    if (used + 3 > output_capacity) {
        return false;
    }
    output[used] = '"';
    output[used + 1] = '}';
    output[used + 2] = '\0';
    *output_length = used + 2;
    return true;
}
