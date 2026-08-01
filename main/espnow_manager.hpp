#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

constexpr size_t kEspNowNameMax = 31;
constexpr size_t kEspNowMaxPeers = 20;

enum class EspNowRate : uint8_t {
    Auto = 0,
    Lr500 = 1,
    Lr250 = 2,
};

struct EspNowPeerSettings {
    char name[kEspNowNameMax + 1];
    uint8_t mac[6];
};

struct EspNowSettings {
    char name[kEspNowNameMax + 1];
    uint8_t channel;
    EspNowRate rate;
    size_t peer_count;
    EspNowPeerSettings peers[kEspNowMaxPeers];
    bool gateway_enabled;
    uint32_t gateway_ipv4;
    uint16_t gateway_port;
};

struct EspNowCounters {
    uint32_t report_cycles;
    uint32_t report_cycles_dropped;
    uint32_t tx_queued;
    uint32_t tx_success;
    uint32_t tx_failed;
    uint32_t tx_timeout;
    uint32_t rx_received;
    uint32_t rx_dropped;
    uint32_t gateway_forwarded;
    uint32_t gateway_dropped;
};

struct EspNowStatus {
    bool initialized;
    bool radio_enabled;
    esp_err_t last_error;
    uint8_t station_mac[6];
    EspNowSettings settings;
    EspNowCounters counters;
};

esp_err_t espnow_manager_init(void);
esp_err_t espnow_manager_get_status(EspNowStatus *status);
esp_err_t espnow_manager_set_name(const char *name);
esp_err_t espnow_manager_set_peer(const char *name, const char *mac_or_none);
esp_err_t espnow_manager_set_channel(uint8_t channel);
esp_err_t espnow_manager_set_rate(EspNowRate rate);
esp_err_t espnow_manager_set_gateway(const char *ipv4, uint16_t port);
esp_err_t espnow_manager_clear_gateway(void);
const char *espnow_rate_name(EspNowRate rate);
bool espnow_rate_parse(const char *text, EspNowRate *rate);

// Nonblocking and coalescing: at most one report cycle waits behind the active
// one. Intended for the end of a BLE scan/probe cycle.
void espnow_manager_publish_reports(void);
