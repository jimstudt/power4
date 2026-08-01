#include "espnow_manager.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "espnow_protocol.hpp"
#include "ethernet_manager.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "state_report.hpp"

namespace {

constexpr const char *kTag = "espnow";
constexpr const char *kNamespace = "espnow";
constexpr const char *kConfigKey = "config";
constexpr uint32_t kConfigMagic = 0x5034454e;  // P4EN
constexpr uint16_t kConfigVersion = 1;
// Four slots match the deliberately small Wi-Fi RX window below. Dropping a
// telemetry frame under a burst is preferable to permanently reserving another
// 6 KiB of internal RAM that the policy interpreter cannot then use.
constexpr size_t kReceiveSlotCount = 4;
constexpr size_t kSendResultCount = 8;
constexpr TickType_t kMutexTicks = pdMS_TO_TICKS(2000);
constexpr TickType_t kSendCallbackTicks = pdMS_TO_TICKS(250);
constexpr TickType_t kWorkerPollTicks = pdMS_TO_TICKS(100);
constexpr uint32_t kWorkerStackBytes = 6144;

struct StoredPeer {
    char name[kEspNowNameMax + 1];
    uint8_t mac[6];
    uint8_t reserved[2];
};

struct StoredConfig {
    uint32_t magic;
    uint16_t version;
    uint8_t channel;
    uint8_t peer_count;
    uint8_t gateway_enabled;
    uint8_t rate;
    uint8_t reserved[2];
    char name[kEspNowNameMax + 1];
    uint32_t gateway_ipv4;
    uint16_t gateway_port;
    uint16_t reserved2;
    StoredPeer peers[kEspNowMaxPeers];
};

struct ReceivedFrame {
    EspNowRadioMetadata metadata;
    uint16_t length;
    uint8_t data[kEspNowProtocolMaxPayloadBytes];
};

struct SendResult {
    uint8_t mac[6];
    esp_now_send_status_t status;
};

StaticSemaphore_t g_mutex_storage = {};
SemaphoreHandle_t g_mutex = nullptr;
portMUX_TYPE g_counter_mux = portMUX_INITIALIZER_UNLOCKED;
EspNowSettings g_settings = {};
EspNowCounters g_counters = {};
uint8_t g_station_mac[6] = {};
esp_err_t g_last_error = ESP_OK;
bool g_initialized = false;
bool g_wifi_initialized = false;
bool g_wifi_started = false;
bool g_espnow_started = false;
uint32_t g_generation = 0;

ReceivedFrame g_receive_slots[kReceiveSlotCount] = {};
StaticQueue_t g_free_queue_control = {};
StaticQueue_t g_ready_queue_control = {};
StaticQueue_t g_send_queue_control = {};
uint8_t g_free_queue_storage[kReceiveSlotCount * sizeof(uint8_t)] = {};
uint8_t g_ready_queue_storage[kReceiveSlotCount * sizeof(uint8_t)] = {};
uint8_t g_send_queue_storage[kSendResultCount * sizeof(SendResult)] = {};
QueueHandle_t g_free_queue = nullptr;
QueueHandle_t g_ready_queue = nullptr;
QueueHandle_t g_send_queue = nullptr;
TaskHandle_t g_worker_task = nullptr;
bool g_report_pending = false;

void increment(uint32_t EspNowCounters::*member)
{
    portENTER_CRITICAL(&g_counter_mux);
    ++(g_counters.*member);
    portEXIT_CRITICAL(&g_counter_mux);
}

bool mac_equal(const uint8_t left[6], const uint8_t right[6])
{
    return memcmp(left, right, 6) == 0;
}

bool valid_unicast_mac(const uint8_t mac[6])
{
    static const uint8_t zero[6] = {};
    return !mac_equal(mac, zero) && (mac[0] & 0x01U) == 0 && !mac_equal(mac, g_station_mac);
}

bool valid_gateway(uint32_t address)
{
    const uint32_t host = ntohl(address);
    const uint8_t first = static_cast<uint8_t>(host >> 24);
    return host != 0 && host != UINT32_MAX && first > 0 && first < 224;
}

esp_err_t validate_settings(const EspNowSettings &settings)
{
    if (settings.channel > 14 || settings.rate > EspNowRate::Lr250 ||
        settings.peer_count > kEspNowMaxPeers ||
        (settings.channel != 0 && !espnow_protocol_valid_name(settings.name)) ||
        (settings.name[0] != '\0' && !espnow_protocol_valid_name(settings.name))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (settings.gateway_enabled &&
        (!valid_gateway(settings.gateway_ipv4) || settings.gateway_port == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < settings.peer_count; ++i) {
        if (!espnow_protocol_valid_name(settings.peers[i].name) ||
            !valid_unicast_mac(settings.peers[i].mac)) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(settings.peers[i].name, settings.peers[j].name) == 0 ||
                mac_equal(settings.peers[i].mac, settings.peers[j].mac)) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

void to_stored(const EspNowSettings &settings, StoredConfig *stored)
{
    *stored = {};
    stored->magic = kConfigMagic;
    stored->version = kConfigVersion;
    stored->channel = settings.channel;
    stored->peer_count = static_cast<uint8_t>(settings.peer_count);
    stored->gateway_enabled = settings.gateway_enabled ? 1 : 0;
    stored->rate = static_cast<uint8_t>(settings.rate);
    memcpy(stored->name, settings.name, sizeof(stored->name));
    stored->gateway_ipv4 = settings.gateway_ipv4;
    stored->gateway_port = settings.gateway_port;
    for (size_t i = 0; i < settings.peer_count; ++i) {
        memcpy(stored->peers[i].name, settings.peers[i].name, sizeof(stored->peers[i].name));
        memcpy(stored->peers[i].mac, settings.peers[i].mac, sizeof(stored->peers[i].mac));
    }
}

esp_err_t from_stored(const StoredConfig &stored, EspNowSettings *settings)
{
    if (stored.magic != kConfigMagic || stored.version != kConfigVersion ||
        stored.peer_count > kEspNowMaxPeers || stored.gateway_enabled > 1 ||
        stored.rate > static_cast<uint8_t>(EspNowRate::Lr250) ||
        stored.name[kEspNowNameMax] != '\0') {
        return ESP_ERR_INVALID_VERSION;
    }
    *settings = {};
    memcpy(settings->name, stored.name, sizeof(settings->name));
    settings->channel = stored.channel;
    settings->rate = static_cast<EspNowRate>(stored.rate);
    settings->peer_count = stored.peer_count;
    settings->gateway_enabled = stored.gateway_enabled != 0;
    settings->gateway_ipv4 = stored.gateway_ipv4;
    settings->gateway_port = stored.gateway_port;
    for (size_t i = 0; i < settings->peer_count; ++i) {
        if (stored.peers[i].name[kEspNowNameMax] != '\0') {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(settings->peers[i].name,
               stored.peers[i].name,
               sizeof(settings->peers[i].name));
        memcpy(settings->peers[i].mac, stored.peers[i].mac, sizeof(settings->peers[i].mac));
    }
    return validate_settings(*settings);
}

esp_err_t load_settings(EspNowSettings *settings)
{
    *settings = {};
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    StoredConfig *stored = static_cast<StoredConfig *>(calloc(1, sizeof(StoredConfig)));
    if (stored == nullptr) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    size_t length = sizeof(*stored);
    err = nvs_get_blob(handle, kConfigKey, stored, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        free(stored);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        free(stored);
        return err;
    }
    if (length != sizeof(*stored)) {
        free(stored);
        return ESP_ERR_INVALID_SIZE;
    }
    err = from_stored(*stored, settings);
    free(stored);
    return err;
}

esp_err_t save_settings(const EspNowSettings &settings)
{
    StoredConfig *stored = static_cast<StoredConfig *>(calloc(1, sizeof(StoredConfig)));
    if (stored == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    to_stored(settings, stored);
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, kConfigKey, stored, sizeof(*stored));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    free(stored);
    return err;
}

void send_callback(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    if (info == nullptr || info->des_addr == nullptr || g_send_queue == nullptr) {
        return;
    }
    SendResult result = {};
    memcpy(result.mac, info->des_addr, sizeof(result.mac));
    result.status = status;
    (void)xQueueSend(g_send_queue, &result, 0);
}

void receive_callback(const esp_now_recv_info_t *info, const uint8_t *data, int length)
{
    if (info == nullptr || info->src_addr == nullptr || info->des_addr == nullptr ||
        data == nullptr || length < 0 ||
        static_cast<size_t>(length) > kEspNowProtocolMaxPayloadBytes) {
        increment(&EspNowCounters::rx_dropped);
        return;
    }
    increment(&EspNowCounters::rx_received);
    uint8_t slot = 0;
    if (xQueueReceive(g_free_queue, &slot, 0) != pdTRUE) {
        increment(&EspNowCounters::rx_dropped);
        return;
    }

    ReceivedFrame &frame = g_receive_slots[slot];
    frame.metadata = {};
    memcpy(frame.metadata.source_mac, info->src_addr, 6);
    memcpy(frame.metadata.destination_mac, info->des_addr, 6);
    if (info->rx_ctrl != nullptr) {
        frame.metadata.rssi_dbm = info->rx_ctrl->rssi;
        frame.metadata.channel = info->rx_ctrl->channel;
        frame.metadata.rate = info->rx_ctrl->rate;
        frame.metadata.signal_mode = info->rx_ctrl->sig_mode;
        frame.metadata.mcs = info->rx_ctrl->mcs;
        frame.metadata.noise_floor_dbm = info->rx_ctrl->noise_floor;
    }
    frame.length = static_cast<uint16_t>(length);
    memcpy(frame.data, data, frame.length);
    if (xQueueSend(g_ready_queue, &slot, 0) != pdTRUE) {
        increment(&EspNowCounters::rx_dropped);
        (void)xQueueSend(g_free_queue, &slot, 0);
    }
}

void stop_radio_locked(void)
{
    if (g_espnow_started) {
        (void)esp_now_unregister_recv_cb();
        (void)esp_now_unregister_send_cb();
        (void)esp_now_deinit();
        g_espnow_started = false;
    }
    if (g_wifi_started) {
        (void)esp_wifi_stop();
        g_wifi_started = false;
    }
    if (g_wifi_initialized) {
        (void)esp_wifi_deinit();
        g_wifi_initialized = false;
    }
}

esp_err_t configure_peer_rate(const uint8_t mac[6], EspNowRate rate)
{
    if (rate == EspNowRate::Auto) {
        return ESP_OK;
    }
    esp_now_rate_config_t config = {};
    config.phymode = WIFI_PHY_MODE_LR;
    config.rate = rate == EspNowRate::Lr250 ? WIFI_PHY_RATE_LORA_250K
                                            : WIFI_PHY_RATE_LORA_500K;
    return esp_now_set_peer_rate_config(mac, &config);
}

esp_err_t start_radio_locked(const EspNowSettings &settings)
{
    if (settings.channel == 0) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    // This radio never associates and ESP-NOW sends exactly one frame at a
    // time. ESP-IDF's defaults are sized for high-throughput Wi-Fi: ten 1.6 KiB
    // permanent RX buffers, 32-entry dynamic RX/TX limits, AMPDU windows, and
    // seven encrypted ESP-NOW peers. Keep only a small bounded window for our
    // unencrypted telemetry traffic.
    config.static_rx_buf_num = 4;
    config.dynamic_rx_buf_num = 4;
    config.dynamic_tx_buf_num = 4;
    config.rx_mgmt_buf_num = 2;
    config.mgmt_sbuf_num = 6;
    config.ampdu_rx_enable = 0;
    config.ampdu_tx_enable = 0;
    config.nvs_enable = 0;
    config.espnow_max_encrypt_num = 0;
    if ((err = esp_wifi_init(&config)) != ESP_OK) {
        return err;
    }
    g_wifi_initialized = true;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        stop_radio_locked();
        return err;
    }
    g_wifi_started = true;
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK ||
        // Always accept both standard and Espressif LR frames. The configured
        // rate controls only this unit's outbound frames, so opposite
        // directions may select different rates.
        (err = esp_wifi_set_protocol(WIFI_IF_STA,
                                     WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                                         WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR)) != ESP_OK ||
        (err = esp_wifi_set_channel(settings.channel, WIFI_SECOND_CHAN_NONE)) != ESP_OK ||
        (err = esp_now_init()) != ESP_OK) {
        stop_radio_locked();
        return err;
    }
    g_espnow_started = true;
    if ((err = esp_now_register_send_cb(send_callback)) != ESP_OK ||
        (err = esp_now_register_recv_cb(receive_callback)) != ESP_OK) {
        stop_radio_locked();
        return err;
    }
    for (size_t i = 0; i < settings.peer_count; ++i) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, settings.peers[i].mac, sizeof(peer.peer_addr));
        peer.channel = settings.channel;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        err = esp_now_add_peer(&peer);
        if (err == ESP_OK) {
            err = configure_peer_rate(peer.peer_addr, settings.rate);
        }
        if (err != ESP_OK) {
            stop_radio_locked();
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t apply_settings(const EspNowSettings &requested, bool force_radio = false)
{
    esp_err_t err = validate_settings(requested);
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(g_mutex, kMutexTicks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    EspNowSettings *previous = static_cast<EspNowSettings *>(malloc(sizeof(EspNowSettings)));
    if (previous == nullptr) {
        xSemaphoreGive(g_mutex);
        return ESP_ERR_NO_MEM;
    }
    *previous = g_settings;
    const bool radio_changed = force_radio || requested.channel != previous->channel ||
                               requested.rate != previous->rate ||
                               requested.peer_count != previous->peer_count ||
                               memcmp(requested.peers, previous->peers, sizeof(requested.peers)) != 0;
    if (radio_changed) {
        stop_radio_locked();
        err = start_radio_locked(requested);
        if (err != ESP_OK) {
            stop_radio_locked();
            const esp_err_t restore_err = start_radio_locked(*previous);
            g_last_error = err;
            if (restore_err != ESP_OK) {
                ESP_LOGE(kTag, "failed to restore previous radio settings: %s", esp_err_to_name(restore_err));
            }
            free(previous);
            xSemaphoreGive(g_mutex);
            return err;
        }
    }
    err = save_settings(requested);
    if (err != ESP_OK) {
        if (radio_changed) {
            stop_radio_locked();
            const esp_err_t restore_err = start_radio_locked(*previous);
            if (restore_err != ESP_OK) {
                ESP_LOGE(kTag,
                         "failed to restore previous radio settings after NVS error: %s",
                         esp_err_to_name(restore_err));
            }
        }
        g_last_error = err;
        free(previous);
        xSemaphoreGive(g_mutex);
        return err;
    }
    g_settings = requested;
    ++g_generation;
    if (requested.channel == 0 || g_espnow_started) {
        g_last_error = ESP_OK;
    }
    free(previous);
    xSemaphoreGive(g_mutex);
    return ESP_OK;
}

esp_err_t settings_snapshot_alloc(EspNowSettings **snapshot)
{
    if (snapshot == nullptr || g_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    *snapshot = static_cast<EspNowSettings *>(malloc(sizeof(EspNowSettings)));
    if (*snapshot == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(g_mutex, kMutexTicks) != pdTRUE) {
        free(*snapshot);
        *snapshot = nullptr;
        return ESP_ERR_TIMEOUT;
    }
    **snapshot = g_settings;
    xSemaphoreGive(g_mutex);
    return ESP_OK;
}

bool send_frame(const uint8_t peer[6],
                const uint8_t *frame,
                size_t length,
                uint32_t generation)
{
    if (xSemaphoreTake(g_mutex, kMutexTicks) != pdTRUE) {
        increment(&EspNowCounters::tx_timeout);
        return false;
    }
    if (!g_espnow_started || generation != g_generation) {
        xSemaphoreGive(g_mutex);
        return false;
    }
    SendResult stale = {};
    while (xQueueReceive(g_send_queue, &stale, 0) == pdTRUE) {
    }
    const esp_err_t err = esp_now_send(peer, frame, length);
    if (err != ESP_OK) {
        increment(&EspNowCounters::tx_failed);
        xSemaphoreGive(g_mutex);
        return false;
    }
    increment(&EspNowCounters::tx_queued);

    const TickType_t deadline = xTaskGetTickCount() + kSendCallbackTicks;
    while (true) {
        const TickType_t now = xTaskGetTickCount();
        const TickType_t remaining = static_cast<int32_t>(deadline - now) > 0 ? deadline - now : 0;
        SendResult result = {};
        if (xQueueReceive(g_send_queue, &result, remaining) != pdTRUE) {
            increment(&EspNowCounters::tx_timeout);
            xSemaphoreGive(g_mutex);
            return false;
        }
        if (!mac_equal(result.mac, peer)) {
            if (remaining == 0) {
                increment(&EspNowCounters::tx_timeout);
                xSemaphoreGive(g_mutex);
                return false;
            }
            continue;
        }
        if (result.status == ESP_NOW_SEND_SUCCESS) {
            increment(&EspNowCounters::tx_success);
            xSemaphoreGive(g_mutex);
            return true;
        }
        increment(&EspNowCounters::tx_failed);
        xSemaphoreGive(g_mutex);
        return false;
    }
}

void send_report_cycle(char *wire, EspNowSettings *snapshot)
{
    uint32_t generation = 0;
    if (xSemaphoreTake(g_mutex, kMutexTicks) != pdTRUE) {
        increment(&EspNowCounters::report_cycles_dropped);
        return;
    }
    *snapshot = g_settings;
    generation = g_generation;
    const bool ready = g_espnow_started && snapshot->channel != 0 &&
                       espnow_protocol_valid_name(snapshot->name);
    xSemaphoreGive(g_mutex);
    if (!ready || snapshot->peer_count == 0) {
        return;
    }

    increment(&EspNowCounters::report_cycles);
    bool peer_failed[kEspNowMaxPeers] = {};
    constexpr StateReportKind reports[] = {
        StateReportKind::Batteries,
        StateReportKind::Banks,
        StateReportKind::Relays,
        StateReportKind::Inputs,
    };
    for (StateReportKind kind : reports) {
        char *json = nullptr;
        size_t json_length = 0;
        const esp_err_t build_err = state_report_build(kind, &json, &json_length);
        if (build_err != ESP_OK) {
            ESP_LOGW(kTag,
                     "cannot build %s: %s",
                     state_report_frame_type(kind),
                     esp_err_to_name(build_err));
            continue;
        }
        const uint32_t message_id = esp_random();
        const size_t fragments = espnow_protocol_fragment_count(json_length);
        for (size_t peer_index = 0; peer_index < snapshot->peer_count; ++peer_index) {
            if (peer_failed[peer_index]) {
                continue;
            }
            for (size_t fragment_index = 0; fragment_index < fragments; ++fragment_index) {
                const size_t offset = fragment_index * kEspNowProtocolReportChunkBytes;
                const size_t remaining = json_length - offset;
                const size_t fragment_length =
                    remaining < kEspNowProtocolReportChunkBytes
                        ? remaining
                        : kEspNowProtocolReportChunkBytes;
                size_t wire_length = 0;
                if (!espnow_protocol_build_report_frame(
                        snapshot->name,
                        state_report_frame_type(kind),
                        message_id,
                        fragment_index,
                        fragments,
                        json_length,
                        reinterpret_cast<const uint8_t *>(json) + offset,
                        fragment_length,
                        wire,
                        kEspNowProtocolFrameMaxBytes + 1,
                        &wire_length)) {
                    ESP_LOGE(kTag, "cannot encode %s fragment", state_report_frame_type(kind));
                    peer_failed[peer_index] = true;
                    break;
                }
                if (!send_frame(snapshot->peers[peer_index].mac,
                                reinterpret_cast<const uint8_t *>(wire),
                                wire_length,
                                generation)) {
                    char mac[18] = {};
                    espnow_protocol_format_mac(snapshot->peers[peer_index].mac, mac);
                    ESP_LOGW(kTag,
                             "report delivery stopped for peer %s (%s)",
                             snapshot->peers[peer_index].name,
                             mac);
                    peer_failed[peer_index] = true;
                    break;
                }
            }
        }
        free(json);
    }
}

void forward_received(const ReceivedFrame &frame, char *gateway_json, int *udp_socket)
{
    char source[18] = {};
    espnow_protocol_format_mac(frame.metadata.source_mac, source);
    ESP_LOGI(kTag,
             "received from %s size=%u rssi=%d channel=%u rate=%u sig_mode=%u mcs=%u noise=%d",
             source,
             frame.length,
             frame.metadata.rssi_dbm,
             frame.metadata.channel,
             frame.metadata.rate,
             frame.metadata.signal_mode,
             frame.metadata.mcs,
             frame.metadata.noise_floor_dbm);

    bool gateway_enabled = false;
    uint32_t gateway_ipv4 = 0;
    uint16_t gateway_port = 0;
    if (xSemaphoreTake(g_mutex, kMutexTicks) != pdTRUE) {
        increment(&EspNowCounters::gateway_dropped);
        return;
    }
    gateway_enabled = g_settings.gateway_enabled;
    gateway_ipv4 = g_settings.gateway_ipv4;
    gateway_port = g_settings.gateway_port;
    xSemaphoreGive(g_mutex);
    if (!gateway_enabled) {
        return;
    }

    EthernetStatus ethernet = {};
    if (ethernet_manager_get_status(&ethernet) != ESP_OK || !ethernet.initialized ||
        !ethernet.has_ip) {
        increment(&EspNowCounters::gateway_dropped);
        return;
    }
    size_t gateway_length = 0;
    if (!espnow_protocol_build_gateway_frame(frame.metadata,
                                             frame.data,
                                             frame.length,
                                             gateway_json,
                                             kEspNowProtocolGatewayMaxBytes + 1,
                                             &gateway_length)) {
        increment(&EspNowCounters::gateway_dropped);
        return;
    }
    if (*udp_socket < 0) {
        *udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (*udp_socket < 0) {
            increment(&EspNowCounters::gateway_dropped);
            return;
        }
        const int flags = fcntl(*udp_socket, F_GETFL, 0);
        if (flags < 0 || fcntl(*udp_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(*udp_socket);
            *udp_socket = -1;
            increment(&EspNowCounters::gateway_dropped);
            return;
        }
    }
    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(gateway_port);
    destination.sin_addr.s_addr = gateway_ipv4;
    const ssize_t sent = sendto(*udp_socket,
                                gateway_json,
                                gateway_length,
                                0,
                                reinterpret_cast<const sockaddr *>(&destination),
                                sizeof(destination));
    if (sent == static_cast<ssize_t>(gateway_length)) {
        increment(&EspNowCounters::gateway_forwarded);
    } else {
        increment(&EspNowCounters::gateway_dropped);
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            close(*udp_socket);
            *udp_socket = -1;
        }
    }
}

void worker_task(void *)
{
    char *wire = static_cast<char *>(malloc(kEspNowProtocolFrameMaxBytes + 1));
    char *gateway = static_cast<char *>(malloc(kEspNowProtocolGatewayMaxBytes + 1));
    EspNowSettings *snapshot = static_cast<EspNowSettings *>(malloc(sizeof(EspNowSettings)));
    if (wire == nullptr || gateway == nullptr || snapshot == nullptr) {
        ESP_LOGE(kTag, "worker scratch allocation failed");
        free(wire);
        free(gateway);
        free(snapshot);
        vTaskDelete(nullptr);
        return;
    }
    int udp_socket = -1;
    while (true) {
        uint8_t slot = 0;
        if (xQueueReceive(g_ready_queue, &slot, kWorkerPollTicks) == pdTRUE) {
            forward_received(g_receive_slots[slot], gateway, &udp_socket);
            if (xQueueSend(g_free_queue, &slot, 0) != pdTRUE) {
                ESP_LOGE(kTag, "receive free-slot queue invariant failed");
            }
            for (size_t drained = 1;
                 drained < kReceiveSlotCount && xQueueReceive(g_ready_queue, &slot, 0) == pdTRUE;
                 ++drained) {
                forward_received(g_receive_slots[slot], gateway, &udp_socket);
                if (xQueueSend(g_free_queue, &slot, 0) != pdTRUE) {
                    ESP_LOGE(kTag, "receive free-slot queue invariant failed");
                }
            }
        }
        if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            portENTER_CRITICAL(&g_counter_mux);
            g_report_pending = false;
            portEXIT_CRITICAL(&g_counter_mux);
            send_report_cycle(wire, snapshot);
        }
    }
}

}  // namespace

esp_err_t espnow_manager_init(void)
{
    if (g_initialized) {
        return ESP_OK;
    }
    if (esp_read_mac(g_station_mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return ESP_FAIL;
    }
    g_mutex = xSemaphoreCreateMutexStatic(&g_mutex_storage);
    if (g_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    g_free_queue = xQueueCreateStatic(kReceiveSlotCount,
                                      sizeof(uint8_t),
                                      g_free_queue_storage,
                                      &g_free_queue_control);
    g_ready_queue = xQueueCreateStatic(kReceiveSlotCount,
                                       sizeof(uint8_t),
                                       g_ready_queue_storage,
                                       &g_ready_queue_control);
    g_send_queue = xQueueCreateStatic(kSendResultCount,
                                      sizeof(SendResult),
                                      g_send_queue_storage,
                                      &g_send_queue_control);
    if (g_free_queue == nullptr || g_ready_queue == nullptr || g_send_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t slot = 0; slot < kReceiveSlotCount; ++slot) {
        (void)xQueueSend(g_free_queue, &slot, 0);
    }

    esp_err_t err = load_settings(&g_settings);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "stored configuration invalid; ESP-NOW disabled: %s", esp_err_to_name(err));
        g_settings = {};
        g_last_error = err;
    }
    const BaseType_t created = xTaskCreate(worker_task,
                                           "espnow_worker",
                                           kWorkerStackBytes,
                                           nullptr,
                                           4,
                                           &g_worker_task);
    if (created != pdPASS) {
        g_worker_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    if (g_settings.channel != 0) {
        err = start_radio_locked(g_settings);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "radio unavailable: %s", esp_err_to_name(err));
            g_last_error = err;
        }
    }
    g_initialized = true;
    char mac[18] = {};
    espnow_protocol_format_mac(g_station_mac, mac);
    ESP_LOGI(kTag,
             "initialized station_mac=%s channel=%s",
             mac,
             g_settings.channel == 0 ? "off" : "configured");
    return ESP_OK;
}

esp_err_t espnow_manager_get_status(EspNowStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_initialized || g_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(g_mutex, kMutexTicks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *status = {};
    status->initialized = g_initialized;
    status->radio_enabled = g_espnow_started;
    status->last_error = g_last_error;
    memcpy(status->station_mac, g_station_mac, sizeof(status->station_mac));
    status->settings = g_settings;
    xSemaphoreGive(g_mutex);
    portENTER_CRITICAL(&g_counter_mux);
    status->counters = g_counters;
    portEXIT_CRITICAL(&g_counter_mux);
    return ESP_OK;
}

esp_err_t espnow_manager_set_name(const char *name)
{
    if (!espnow_protocol_valid_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    EspNowSettings *requested = nullptr;
    esp_err_t err = settings_snapshot_alloc(&requested);
    if (err != ESP_OK) {
        return err;
    }
    strncpy(requested->name, name, sizeof(requested->name));
    requested->name[kEspNowNameMax] = '\0';
    err = apply_settings(*requested);
    free(requested);
    return err;
}

esp_err_t espnow_manager_set_peer(const char *name, const char *mac_or_none)
{
    if (!espnow_protocol_valid_name(name) || mac_or_none == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    EspNowSettings *requested = nullptr;
    esp_err_t err = settings_snapshot_alloc(&requested);
    if (err != ESP_OK) {
        return err;
    }
    size_t index = requested->peer_count;
    for (size_t i = 0; i < requested->peer_count; ++i) {
        if (strcmp(requested->peers[i].name, name) == 0) {
            index = i;
            break;
        }
    }
    if (strcmp(mac_or_none, "none") == 0) {
        if (index == requested->peer_count) {
            free(requested);
            return ESP_ERR_NOT_FOUND;
        }
        for (size_t i = index + 1; i < requested->peer_count; ++i) {
            requested->peers[i - 1] = requested->peers[i];
        }
        requested->peers[--requested->peer_count] = {};
        err = apply_settings(*requested);
        free(requested);
        return err;
    }

    uint8_t mac[6] = {};
    if (!espnow_protocol_parse_mac(mac_or_none, mac) || !valid_unicast_mac(mac)) {
        free(requested);
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < requested->peer_count; ++i) {
        if (i != index && mac_equal(requested->peers[i].mac, mac)) {
            free(requested);
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (index == requested->peer_count) {
        if (requested->peer_count == kEspNowMaxPeers) {
            free(requested);
            return ESP_ERR_INVALID_SIZE;
        }
        ++requested->peer_count;
    }
    requested->peers[index] = {};
    strncpy(requested->peers[index].name, name, sizeof(requested->peers[index].name));
    memcpy(requested->peers[index].mac, mac, sizeof(requested->peers[index].mac));
    err = apply_settings(*requested);
    free(requested);
    return err;
}

esp_err_t espnow_manager_set_channel(uint8_t channel)
{
    if (channel > 14) {
        return ESP_ERR_INVALID_ARG;
    }
    EspNowSettings *requested = nullptr;
    esp_err_t err = settings_snapshot_alloc(&requested);
    if (err != ESP_OK) {
        return err;
    }
    if (channel != 0 && !espnow_protocol_valid_name(requested->name)) {
        free(requested);
        return ESP_ERR_INVALID_ARG;
    }
    requested->channel = channel;
    err = apply_settings(*requested, true);
    free(requested);
    return err;
}

esp_err_t espnow_manager_set_rate(EspNowRate rate)
{
    if (rate > EspNowRate::Lr250) {
        return ESP_ERR_INVALID_ARG;
    }
    EspNowSettings *requested = nullptr;
    esp_err_t err = settings_snapshot_alloc(&requested);
    if (err != ESP_OK) {
        return err;
    }
    requested->rate = rate;
    err = apply_settings(*requested);
    free(requested);
    return err;
}

esp_err_t espnow_manager_set_gateway(const char *ipv4, uint16_t port)
{
    if (ipv4 == nullptr || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    in_addr address = {};
    if (inet_pton(AF_INET, ipv4, &address) != 1 || !valid_gateway(address.s_addr)) {
        return ESP_ERR_INVALID_ARG;
    }
    EspNowSettings *requested = nullptr;
    esp_err_t err = settings_snapshot_alloc(&requested);
    if (err != ESP_OK) {
        return err;
    }
    requested->gateway_enabled = true;
    requested->gateway_ipv4 = address.s_addr;
    requested->gateway_port = port;
    err = apply_settings(*requested);
    free(requested);
    return err;
}

esp_err_t espnow_manager_clear_gateway(void)
{
    EspNowSettings *requested = nullptr;
    esp_err_t err = settings_snapshot_alloc(&requested);
    if (err != ESP_OK) {
        return err;
    }
    requested->gateway_enabled = false;
    requested->gateway_ipv4 = 0;
    requested->gateway_port = 0;
    err = apply_settings(*requested);
    free(requested);
    return err;
}

void espnow_manager_publish_reports(void)
{
    if (!g_initialized || !g_espnow_started || g_worker_task == nullptr) {
        return;
    }
    bool notify = false;
    portENTER_CRITICAL(&g_counter_mux);
    if (g_report_pending) {
        ++g_counters.report_cycles_dropped;
    } else {
        g_report_pending = true;
        notify = true;
    }
    portEXIT_CRITICAL(&g_counter_mux);
    if (notify) {
        xTaskNotifyGive(g_worker_task);
    }
}

const char *espnow_rate_name(EspNowRate rate)
{
    switch (rate) {
    case EspNowRate::Auto:
        return "auto";
    case EspNowRate::Lr500:
        return "lr-500";
    case EspNowRate::Lr250:
        return "lr-250";
    }
    return "invalid";
}

bool espnow_rate_parse(const char *text, EspNowRate *rate)
{
    if (text == nullptr || rate == nullptr) {
        return false;
    }
    if (strcmp(text, "auto") == 0) {
        *rate = EspNowRate::Auto;
    } else if (strcmp(text, "lr-500") == 0) {
        *rate = EspNowRate::Lr500;
    } else if (strcmp(text, "lr-250") == 0) {
        *rate = EspNowRate::Lr250;
    } else {
        return false;
    }
    return true;
}
