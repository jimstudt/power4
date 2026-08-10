#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "jbd_protocol.hpp"

namespace {

size_t make_response(uint8_t command,
                     uint8_t status,
                     const uint8_t *payload,
                     uint8_t payload_len,
                     uint8_t *frame)
{
    frame[0] = 0xdd;
    frame[1] = command;
    frame[2] = status;
    frame[3] = payload_len;
    if (payload_len > 0) {
        memcpy(frame + 4, payload, payload_len);
    }
    const uint16_t checksum = jbd_checksum(frame + 2, payload_len + 2);
    frame[4 + payload_len] = static_cast<uint8_t>(checksum >> 8);
    frame[5 + payload_len] = static_cast<uint8_t>(checksum);
    frame[6 + payload_len] = 0x77;
    return payload_len + 7;
}

void test_basic_info()
{
    uint8_t payload[] = {
        0x06, 0x0b, 0xff, 0x9c, 0x01, 0xed, 0x01, 0xf4, 0x00, 0x2c,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x80, 0x63,
        0x03, 0x04, 0x02, 0x0b, 0xa0, 0x0b, 0x9d,
    };
    uint8_t frame[64] = {};
    const size_t length = make_response(kJbdBasicInfoCommand, 0, payload, sizeof(payload), frame);

    JbdBasicInfo info = {};
    assert(jbd_parse_basic_info(frame, length, &info));
    assert(fabsf(info.voltage_v - 15.47f) < 0.001f);
    assert(fabsf(info.current_a + 1.0f) < 0.001f);
    assert(info.cycle_count == 44);
    assert(info.protection_status == kJbdProtectionCellUndervoltage);
    assert(info.soc_percent == 99);
    assert(info.cell_count == 4);
    assert(info.temperature_valid);
    assert(fabsf(info.average_temperature_c - 24.35f) < 0.01f);

    payload[16] = 0x00;
    payload[17] = 0x00;
    make_response(kJbdBasicInfoCommand, 0, payload, sizeof(payload), frame);
    assert(jbd_parse_basic_info(frame, length, &info));
    assert(info.protection_status == 0);
    assert((info.protection_status & kJbdProtectionCellUndervoltage) == 0);

    payload[17] = 0x01;
    make_response(kJbdBasicInfoCommand, 0, payload, sizeof(payload), frame);
    assert(jbd_parse_basic_info(frame, length, &info));
    assert(info.protection_status == 0x0001);
    assert((info.protection_status & kJbdProtectionCellUndervoltage) == 0);
}

void test_cell_info_and_stream_finding()
{
    const uint8_t payload[] = {0x0f, 0x23, 0x0f, 0x1c, 0x0f, 0x12, 0x0f, 0x1d};
    uint8_t frame[32] = {};
    const size_t length = make_response(kJbdCellInfoCommand, 0, payload, sizeof(payload), frame);

    JbdCellInfo info = {};
    assert(jbd_parse_cell_info(frame, length, 4, &info));
    assert(info.count == 4);
    assert(info.voltage_mv[0] == 3875);
    assert(info.voltage_mv[2] == 3858);
    assert(!jbd_parse_cell_info(frame, length, 3, &info));
    assert(!jbd_parse_cell_info(frame, length - 1, 4, &info));

    uint8_t stream[40] = {0x01, 0x02, 0x03};
    memcpy(stream + 3, frame, length);
    size_t offset = 0;
    size_t packet_len = 0;
    assert(!jbd_find_packet(stream, 3 + length - 1, kJbdCellInfoCommand, &offset, &packet_len));
    assert(jbd_find_packet(stream, 3 + length, kJbdCellInfoCommand, &offset, &packet_len));
    assert(offset == 3);
    assert(packet_len == length);

    stream[3 + length - 2] ^= 0x01;
    assert(!jbd_find_packet(stream, 3 + length, kJbdCellInfoCommand, &offset, &packet_len));
}

void test_invalid_cell_frames()
{
    uint8_t frame[80] = {};
    const uint8_t odd[] = {0x0f, 0x23, 0x0f};
    size_t length = make_response(kJbdCellInfoCommand, 0, odd, sizeof(odd), frame);
    JbdCellInfo info = {};
    assert(!jbd_parse_cell_info(frame, length, 1, &info));

    length = make_response(kJbdCellInfoCommand, 0, nullptr, 0, frame);
    assert(!jbd_parse_cell_info(frame, length, 0, &info));

    length = make_response(kJbdCellInfoCommand, 0x80, nullptr, 0, frame);
    assert(!jbd_parse_cell_info(frame, length, 0, &info));
}

void test_protection_descriptions()
{
    static constexpr const char *expected[] = {
        "cell overvoltage",
        "cell undervoltage",
        "pack overvoltage",
        "pack undervoltage",
        "charge overtemperature",
        "charge undertemperature",
        "discharge overtemperature",
        "discharge undertemperature",
        "charge overcurrent",
        "discharge overcurrent",
        "short circuit",
        "front-end IC error",
        "charge/discharge FET locked by configuration",
    };
    for (uint8_t bit = 0; bit < sizeof(expected) / sizeof(expected[0]); ++bit) {
        assert(strcmp(jbd_protection_description(bit), expected[bit]) == 0);
    }
    assert(jbd_protection_description(13) == nullptr);
    assert(jbd_protection_description(15) == nullptr);
}

}  // namespace

int main()
{
    test_basic_info();
    test_cell_info_and_stream_finding();
    test_invalid_cell_frames();
    test_protection_descriptions();
    puts("JBD protocol tests: ok");
    return 0;
}
