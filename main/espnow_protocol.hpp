#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t kEspNowProtocolMaxPayloadBytes = 1470;
constexpr size_t kEspNowProtocolFrameMaxBytes = 900;
constexpr size_t kEspNowProtocolReportChunkBytes = 480;
constexpr size_t kEspNowProtocolGatewayMaxBytes =
    320 + (((kEspNowProtocolMaxPayloadBytes + 2) / 3) * 4);

struct EspNowRadioMetadata {
    uint8_t source_mac[6];
    uint8_t destination_mac[6];
    int8_t rssi_dbm;
    uint8_t channel;
    uint8_t rate;
    uint8_t signal_mode;
    uint8_t mcs;
    int8_t noise_floor_dbm;
};

bool espnow_protocol_valid_name(const char *name);
bool espnow_protocol_parse_mac(const char *text, uint8_t mac[6]);
void espnow_protocol_format_mac(const uint8_t mac[6], char output[18]);

size_t espnow_protocol_fragment_count(size_t content_length);
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
                                        size_t *output_length);
bool espnow_protocol_build_gateway_frame(const EspNowRadioMetadata &metadata,
                                         const uint8_t *payload,
                                         size_t payload_length,
                                         char *output,
                                         size_t output_capacity,
                                         size_t *output_length);
