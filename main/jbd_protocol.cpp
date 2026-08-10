#include "jbd_protocol.hpp"

#include <string.h>

namespace {

uint16_t read_be16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) << 8 | static_cast<uint16_t>(data[1]);
}

int16_t read_be16_signed(const uint8_t *data)
{
    return static_cast<int16_t>(read_be16(data));
}

bool checksum_matches(const uint8_t *frame, size_t data_len)
{
    const uint16_t received = read_be16(frame + 4 + data_len);

    // Standard JBD responses checksum status, length, and payload. Keep the
    // command-inclusive form accepted by the existing scanner for fielded
    // firmware variants.
    if (received == jbd_checksum(frame + 2, data_len + 2)) {
        return true;
    }
    return received == jbd_checksum(frame + 1, data_len + 3);
}

bool valid_frame(const uint8_t *frame, size_t length, uint8_t command)
{
    if (frame == nullptr || length < 7 || frame[0] != 0xdd || frame[1] != command) {
        return false;
    }

    const size_t data_len = frame[3];
    const size_t expected_length = data_len + 7;
    return length == expected_length && frame[expected_length - 1] == 0x77 &&
           checksum_matches(frame, data_len);
}

}  // namespace

uint16_t jbd_checksum(const uint8_t *data, size_t length)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum = static_cast<uint16_t>(sum + data[i]);
    }
    return static_cast<uint16_t>(0 - sum);
}

bool jbd_find_packet(const uint8_t *data,
                     size_t length,
                     uint8_t expected_command,
                     size_t *offset,
                     size_t *packet_len)
{
    if (data == nullptr || offset == nullptr || packet_len == nullptr) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        if (data[i] != 0xdd) {
            continue;
        }
        if (length - i < 7) {
            return false;
        }
        if (data[i + 1] != expected_command) {
            continue;
        }

        const size_t total_len = static_cast<size_t>(data[i + 3]) + 7;
        if (length - i < total_len) {
            return false;
        }
        if (!valid_frame(data + i, total_len, expected_command)) {
            continue;
        }

        *offset = i;
        *packet_len = total_len;
        return true;
    }
    return false;
}

bool jbd_parse_basic_info(const uint8_t *frame, size_t length, JbdBasicInfo *info)
{
    if (info == nullptr || !valid_frame(frame, length, kJbdBasicInfoCommand) || frame[2] != 0x00 ||
        frame[3] < 23) {
        return false;
    }

    const uint8_t ntc_count = frame[26];
    const size_t required_data_len = 23 + (static_cast<size_t>(ntc_count) * 2);
    if (frame[3] < required_data_len) {
        return false;
    }

    JbdBasicInfo parsed = {};
    parsed.voltage_v = read_be16(frame + 4) / 100.0f;
    parsed.current_a = read_be16_signed(frame + 6) / 100.0f;
    parsed.residual_ah = read_be16(frame + 8) / 100.0f;
    parsed.nominal_ah = read_be16(frame + 10) / 100.0f;
    parsed.cycle_count = read_be16(frame + 12);
    parsed.protection_status = read_be16(frame + 20);
    parsed.version = frame[22];
    parsed.soc_percent = frame[23];
    parsed.fet_status = frame[24];
    parsed.cell_count = frame[25];
    parsed.ntc_count = ntc_count;

    for (uint8_t i = 0; i < ntc_count; ++i) {
        parsed.average_temperature_c +=
            (static_cast<int32_t>(read_be16(frame + 27 + (i * 2))) - 2731) / 10.0f;
    }
    if (ntc_count > 0) {
        parsed.average_temperature_c /= ntc_count;
        parsed.temperature_valid = true;
    }

    *info = parsed;
    return true;
}

bool jbd_parse_cell_info(const uint8_t *frame,
                         size_t length,
                         uint8_t expected_cell_count,
                         JbdCellInfo *info)
{
    if (info == nullptr || !valid_frame(frame, length, kJbdCellInfoCommand) || frame[2] != 0x00) {
        return false;
    }

    const uint8_t data_len = frame[3];
    if (data_len < 2 || data_len > kJbdMaxCells * 2 || (data_len % 2) != 0) {
        return false;
    }

    const uint8_t count = data_len / 2;
    if (expected_cell_count == 0 || count != expected_cell_count) {
        return false;
    }

    JbdCellInfo parsed = {};
    parsed.count = count;
    for (uint8_t i = 0; i < count; ++i) {
        parsed.voltage_mv[i] = read_be16(frame + 4 + (i * 2));
    }
    *info = parsed;
    return true;
}

const char *jbd_protection_description(uint8_t bit)
{
    static constexpr const char *descriptions[] = {
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

    return bit < sizeof(descriptions) / sizeof(descriptions[0]) ? descriptions[bit] : nullptr;
}
