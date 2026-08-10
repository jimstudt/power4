#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr uint8_t kJbdBasicInfoCommand = 0x03;
constexpr uint8_t kJbdCellInfoCommand = 0x04;
constexpr size_t kJbdMaxCells = 32;
constexpr uint16_t kJbdProtectionCellUndervoltage = 0x0002;

struct JbdBasicInfo {
    float voltage_v;
    float current_a;
    float residual_ah;
    float nominal_ah;
    uint16_t cycle_count;
    uint16_t protection_status;
    uint8_t version;
    uint8_t soc_percent;
    uint8_t fet_status;
    uint8_t cell_count;
    uint8_t ntc_count;
    bool temperature_valid;
    float average_temperature_c;
};

struct JbdCellInfo {
    uint8_t count;
    uint16_t voltage_mv[kJbdMaxCells];
};

uint16_t jbd_checksum(const uint8_t *data, size_t length);
bool jbd_find_packet(const uint8_t *data,
                     size_t length,
                     uint8_t expected_command,
                     size_t *offset,
                     size_t *packet_len);
bool jbd_parse_basic_info(const uint8_t *frame, size_t length, JbdBasicInfo *info);
bool jbd_parse_cell_info(const uint8_t *frame,
                         size_t length,
                         uint8_t expected_cell_count,
                         JbdCellInfo *info);
const char *jbd_protection_description(uint8_t bit);
