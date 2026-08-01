#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "espnow_protocol.hpp"

namespace {

std::string content_field(const char *json)
{
    const char marker[] = "\"content\":\"";
    const char *start = strstr(json, marker);
    assert(start != nullptr);
    start += sizeof(marker) - 1;
    const char *end = strchr(start, '"');
    assert(end != nullptr);
    return std::string(start, static_cast<size_t>(end - start));
}

std::vector<uint8_t> decode_base64(const std::string &text)
{
    const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> output;
    uint32_t accumulator = 0;
    unsigned bits = 0;
    for (char ch : text) {
        if (ch == '=') {
            break;
        }
        const char *position = strchr(alphabet, ch);
        assert(position != nullptr);
        accumulator = (accumulator << 6) | static_cast<uint32_t>(position - alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
        }
    }
    return output;
}

}  // namespace

int main()
{
    assert(espnow_protocol_valid_name("house"));
    assert(espnow_protocol_valid_name("shed-2_main"));
    assert(!espnow_protocol_valid_name(""));
    assert(!espnow_protocol_valid_name("two words"));
    assert(!espnow_protocol_valid_name("abcdefghijklmnopqrstuvwxyz123456"));

    uint8_t mac[6] = {};
    assert(espnow_protocol_parse_mac("01:23:45:67:89:aB", mac));
    const uint8_t expected_mac[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
    assert(memcmp(mac, expected_mac, sizeof(mac)) == 0);
    char formatted[18] = {};
    espnow_protocol_format_mac(mac, formatted);
    assert(strcmp(formatted, "01:23:45:67:89:ab") == 0);
    assert(!espnow_protocol_parse_mac("01-23-45-67-89-ab", mac));

    assert(espnow_protocol_fragment_count(0) == 1);
    assert(espnow_protocol_fragment_count(480) == 1);
    assert(espnow_protocol_fragment_count(481) == 2);

    char frame[kEspNowProtocolFrameMaxBytes + 1] = {};
    size_t frame_length = 0;
    const uint8_t abc[] = {'a', 'b', 'c'};
    assert(espnow_protocol_build_report_frame("house",
                                              "report-batteries",
                                              7,
                                              0,
                                              1,
                                              sizeof(abc),
                                              abc,
                                              sizeof(abc),
                                              frame,
                                              sizeof(frame),
                                              &frame_length));
    assert(frame_length == strlen(frame));
    assert(strcmp(frame,
                  "{\"protocol\":\"power4-espnow\",\"version\":1,"
                  "\"name\":\"house\",\"frame_type\":\"report-batteries\","
                  "\"message_id\":7,\"fragment\":1,\"fragments\":1,"
                  "\"content_length\":3,\"content_encoding\":\"base64\","
                  "\"content\":\"YWJj\"}") == 0);
    char too_small[32] = {};
    assert(!espnow_protocol_build_report_frame("house",
                                               "report-batteries",
                                               7,
                                               0,
                                               1,
                                               sizeof(abc),
                                               abc,
                                               sizeof(abc),
                                               too_small,
                                               sizeof(too_small),
                                               &frame_length));

    uint8_t maximum_chunk[kEspNowProtocolReportChunkBytes] = {};
    memset(maximum_chunk, 0xa5, sizeof(maximum_chunk));
    assert(espnow_protocol_build_report_frame("abcdefghijklmnopqrstuvwxyz12345",
                                              "report-batteries",
                                              UINT32_MAX,
                                              8,
                                              9,
                                              4096,
                                              maximum_chunk,
                                              sizeof(maximum_chunk),
                                              frame,
                                              sizeof(frame),
                                              &frame_length));
    assert(frame_length <= kEspNowProtocolFrameMaxBytes);
    assert(!espnow_protocol_build_report_frame("house",
                                               "report-batteries",
                                               1,
                                               0,
                                               1,
                                               sizeof(maximum_chunk) + 1,
                                               maximum_chunk,
                                               sizeof(maximum_chunk) + 1,
                                               frame,
                                               sizeof(frame),
                                               &frame_length));

    std::vector<uint8_t> original(1001);
    for (size_t i = 0; i < original.size(); ++i) {
        original[i] = static_cast<uint8_t>(i & 0xff);
    }
    std::vector<uint8_t> reassembled;
    const size_t part_count = espnow_protocol_fragment_count(original.size());
    assert(part_count == 3);
    for (size_t part = 0; part < part_count; ++part) {
        const size_t offset = part * kEspNowProtocolReportChunkBytes;
        const size_t remaining = original.size() - offset;
        const size_t part_length =
            remaining < kEspNowProtocolReportChunkBytes
                ? remaining
                : kEspNowProtocolReportChunkBytes;
        assert(espnow_protocol_build_report_frame("house",
                                                  "report-banks",
                                                  99,
                                                  part,
                                                  part_count,
                                                  original.size(),
                                                  original.data() + offset,
                                                  part_length,
                                                  frame,
                                                  sizeof(frame),
                                                  &frame_length));
        const std::vector<uint8_t> decoded = decode_base64(content_field(frame));
        reassembled.insert(reassembled.end(), decoded.begin(), decoded.end());
    }
    assert(reassembled == original);

    EspNowRadioMetadata metadata = {};
    memcpy(metadata.source_mac, expected_mac, 6);
    const uint8_t destination[] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
    memcpy(metadata.destination_mac, destination, 6);
    metadata.rssi_dbm = -67;
    metadata.channel = 6;
    metadata.rate = 2;
    metadata.signal_mode = 1;
    metadata.mcs = 3;
    metadata.noise_floor_dbm = -96;
    const uint8_t binary[] = {0x00, 0xff, 0x10};
    char gateway[kEspNowProtocolGatewayMaxBytes + 1] = {};
    size_t gateway_length = 0;
    assert(espnow_protocol_build_gateway_frame(metadata,
                                               binary,
                                               sizeof(binary),
                                               gateway,
                                               sizeof(gateway),
                                               &gateway_length));
    assert(gateway_length == strlen(gateway));
    assert(strstr(gateway, "\"source_mac\":\"01:23:45:67:89:ab\"") != nullptr);
    assert(strstr(gateway, "\"rssi_dbm\":-67") != nullptr);
    assert(content_field(gateway) == "AP8Q");

    uint8_t maximum_payload[kEspNowProtocolMaxPayloadBytes] = {};
    assert(espnow_protocol_build_gateway_frame(metadata,
                                               maximum_payload,
                                               sizeof(maximum_payload),
                                               gateway,
                                               sizeof(gateway),
                                               &gateway_length));
    assert(gateway_length < sizeof(gateway));
    assert(espnow_protocol_build_gateway_frame(metadata,
                                               maximum_payload,
                                               kEspNowProtocolFrameMaxBytes,
                                               gateway,
                                               sizeof(gateway),
                                               &gateway_length));
    assert(gateway_length <= 1472);

    puts("ESP-NOW protocol framing: ok");
    return 0;
}
