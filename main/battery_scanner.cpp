#include "battery_scanner.hpp"

#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "battery_store.hpp"
#include "ble_manager.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

extern "C" {
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"
}

namespace {

constexpr const char *kTag = "battery_scanner";
constexpr uint16_t kJbdServiceUuid = 0xff00;
constexpr uint16_t kJbdNotifyCharacteristicUuid = 0xff01;
constexpr uint16_t kJbdWriteCharacteristicUuid = 0xff02;
constexpr TickType_t kSyncWaitTicks = pdMS_TO_TICKS(30000);
constexpr TickType_t kScanPeriodTicks =
    pdMS_TO_TICKS(CONFIG_POWER4_BATTERY_SCAN_PERIOD_SECONDS * 1000);
constexpr uint32_t kScanDurationMs = CONFIG_POWER4_BATTERY_SCAN_DURATION_SECONDS * 1000;
constexpr size_t kSeenDevicesMax = 32;
constexpr size_t kJbdRxBufferSize = 128;
constexpr TickType_t kInterrogationTimeoutTicks = pdMS_TO_TICKS(5000);
constexpr TickType_t kJbdCommandRetryTicks = pdMS_TO_TICKS(250);

constexpr EventBits_t kScanDoneBit = BIT0;
constexpr EventBits_t kProbeConnectedBit = BIT1;
constexpr EventBits_t kProbeDisconnectedBit = BIT2;
constexpr EventBits_t kProbeServiceDoneBit = BIT3;
constexpr EventBits_t kProbeCharsDoneBit = BIT4;
constexpr EventBits_t kProbeDscDoneBit = BIT5;
constexpr EventBits_t kProbeSubscribeDoneBit = BIT6;
constexpr EventBits_t kProbeWriteDoneBit = BIT7;
constexpr EventBits_t kProbePacketDoneBit = BIT8;
constexpr EventBits_t kProbeFailedBit = BIT9;

struct AdvSummary {
    bool has_jbd_service = false;
    const uint8_t *uuids16 = nullptr;
    uint8_t uuids16_len = 0;
    const uint8_t *sol_uuids16 = nullptr;
    uint8_t sol_uuids16_len = 0;
    const uint8_t *name = nullptr;
    uint8_t name_len = 0;
    bool name_complete = false;
    const uint8_t *svc_data_uuid16 = nullptr;
    uint8_t svc_data_uuid16_len = 0;
    const uint8_t *mfg_data = nullptr;
    uint8_t mfg_data_len = 0;
};

struct SeenReport {
    ble_addr_t addr;
    uint8_t event_type = 0;
};

struct BatteryCandidate {
    ble_addr_t addr;
    int8_t rssi = 0;
    char name[kBatteryNameMax + 1] = {};
};

struct ScanStats {
    uint32_t reports = 0;
    uint32_t adv_reports = 0;
    uint32_t scan_response_reports = 0;
    uint32_t parse_errors = 0;
    uint32_t raw_malformed = 0;
    uint32_t jbd_reports = 0;
    uint32_t printed_reports = 0;
    uint32_t duplicate_jbd_reports = 0;
};

struct ProbeState {
    ble_addr_t addr = {};
    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    uint16_t service_start = 0;
    uint16_t service_end = 0;
    uint16_t ff01_def_handle = 0;
    uint16_t ff01_val_handle = 0;
    uint16_t ff01_properties = 0;
    uint16_t ff02_def_handle = 0;
    uint16_t ff02_val_handle = 0;
    uint16_t ff02_properties = 0;
    uint16_t notify_def_handle = 0;
    uint16_t notify_val_handle = 0;
    uint16_t notify_properties = 0;
    uint16_t write_def_handle = 0;
    uint16_t write_val_handle = 0;
    uint16_t write_properties = 0;
    uint16_t cccd_handle = 0;
    int status = 0;
    uint8_t rx[kJbdRxBufferSize] = {};
    size_t rx_len = 0;
    size_t packet_offset = 0;
    size_t packet_len = 0;
};

TaskHandle_t g_scanner_task = nullptr;
EventGroupHandle_t g_scanner_events = nullptr;
SeenReport g_seen_reports[kSeenDevicesMax] = {};
size_t g_seen_report_count = 0;
BatteryCandidate g_candidates[kSeenDevicesMax] = {};
size_t g_candidate_count = 0;
ScanStats g_scan_stats = {};
ProbeState g_probe = {};
bool g_verbose_battery_scanning = false;

void format_addr(const ble_addr_t &addr, char *buffer, size_t buffer_size)
{
    snprintf(buffer,
             buffer_size,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             addr.val[5],
             addr.val[4],
             addr.val[3],
             addr.val[2],
             addr.val[1],
             addr.val[0]);
}

bool has_uuid16(const ble_uuid16_t *uuids, uint8_t count, uint16_t value)
{
    for (uint8_t i = 0; i < count; ++i) {
        if (uuids[i].value == value) {
            return true;
        }
    }
    return false;
}

bool has_jbd_service(const ble_hs_adv_fields &fields)
{
    if (has_uuid16(fields.uuids16, fields.num_uuids16, kJbdServiceUuid)) {
        return true;
    }
    if (has_uuid16(fields.sol_uuids16, fields.sol_num_uuids16, kJbdServiceUuid)) {
        return true;
    }

    return fields.svc_data_uuid16_len >= 2 && fields.svc_data_uuid16[0] == 0x00 &&
           fields.svc_data_uuid16[1] == 0xff;
}

uint16_t uuid16_from_le(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

bool uuid16_field_has_service(const uint8_t *data, uint8_t length, uint16_t uuid)
{
    for (uint8_t i = 0; i + 1 < length; i += 2) {
        if (uuid16_from_le(data + i) == uuid) {
            return true;
        }
    }
    return false;
}

bool uuid_is16(const ble_uuid_t *uuid, uint16_t value)
{
    if (uuid == nullptr || uuid->type != BLE_UUID_TYPE_16) {
        return false;
    }

    return reinterpret_cast<const ble_uuid16_t *>(uuid)->value == value;
}

uint16_t read_be16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) << 8 | static_cast<uint16_t>(data[1]);
}

int16_t read_be16_signed(const uint8_t *data)
{
    return static_cast<int16_t>(read_be16(data));
}

uint16_t jbd_checksum(const uint8_t *data, size_t length)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum = static_cast<uint16_t>(sum + data[i]);
    }
    return static_cast<uint16_t>(0 - sum);
}

bool jbd_checksum_matches(const uint8_t *frame, size_t start, size_t data_len)
{
    const size_t checksum_offset = start + 4 + data_len;
    const uint16_t received = read_be16(frame + checksum_offset);

    if (received == jbd_checksum(frame + start + 2, data_len + 2)) {
        return true;
    }

    return received == jbd_checksum(frame + start + 1, data_len + 3);
}

bool find_jbd_basic_packet(const uint8_t *data, size_t length, size_t *offset, size_t *packet_len)
{
    for (size_t i = 0; i < length; ++i) {
        if (data[i] != 0xdd) {
            continue;
        }
        if (length - i < 7) {
            return false;
        }
        if (data[i + 1] != 0x03 || data[i + 2] != 0x00) {
            continue;
        }

        const size_t body_len = data[i + 3];
        const size_t total_len = body_len + 7;
        if (length - i < total_len) {
            return false;
        }
        if (data[i + total_len - 1] != 0x77) {
            continue;
        }
        if (!jbd_checksum_matches(data, i, body_len)) {
            continue;
        }

        *offset = i;
        *packet_len = total_len;
        return true;
    }

    return false;
}

bool char_can_notify(uint16_t properties)
{
    return (properties & BLE_GATT_CHR_F_NOTIFY) != 0;
}

bool char_can_write(uint16_t properties)
{
    return (properties & (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP)) != 0;
}

void log_jbd_basic_info(const char *battery_name, const uint8_t *frame, size_t length)
{
    if (length < 27 || frame[3] < 23) {
        ESP_LOGW(kTag, "JBD basic info packet too short to decode: battery=%s len=%u",
                 battery_name,
                 static_cast<unsigned>(length));
        return;
    }

    const float voltage_v = read_be16(frame + 4) / 100.0f;
    const float current_a = read_be16_signed(frame + 6) / 100.0f;
    const float residual_ah = read_be16(frame + 8) / 100.0f;
    const float nominal_ah = read_be16(frame + 10) / 100.0f;
    const uint16_t cycles = read_be16(frame + 12);
    const uint16_t protection = read_be16(frame + 20);
    const uint8_t version = frame[22];
    const uint8_t soc_percent = frame[23];
    const uint8_t fet_status = frame[24];
    const uint8_t cell_count = frame[25];
    const uint8_t ntc_count = frame[26];

    float avg_temp_c = 0.0f;
    uint8_t decoded_temps = 0;
    for (uint8_t i = 0; i < ntc_count; ++i) {
        const size_t offset = 27 + (i * 2);
        if (offset + 1 >= length - 3) {
            break;
        }
        avg_temp_c += (read_be16(frame + offset) - 2731) / 10.0f;
        ++decoded_temps;
    }
    if (decoded_temps > 0) {
        avg_temp_c /= decoded_temps;
    }

    const bool temperature_valid = decoded_temps > 0;
    const esp_err_t err = battery_store_record_observation(battery_name,
                                                           voltage_v,
                                                           current_a,
                                                           soc_percent,
                                                           cycles,
                                                           temperature_valid,
                                                           avg_temp_c);
    if (err != ESP_OK) {
        ESP_LOGW(kTag,
                 "failed to record battery observation: battery=%s err=%s",
                 battery_name,
                 esp_err_to_name(err));
    }

    if (!g_verbose_battery_scanning) {
        return;
    }

    if (decoded_temps > 0) {
        ESP_LOGI(kTag,
                 "JBD basic info: battery=%s voltage=%.2fV current=%.2fA soc=%u%% residual=%.2fAh nominal=%.2fAh cycles=%u protection=0x%04x fet=0x%02x cells=%u ntc=%u avg_temp=%.1fC version=%u",
                 battery_name,
                 voltage_v,
                 current_a,
                 soc_percent,
                 residual_ah,
                 nominal_ah,
                 cycles,
                 protection,
                 fet_status,
                 cell_count,
                 ntc_count,
                 avg_temp_c,
                 version);
    } else {
        ESP_LOGI(kTag,
                 "JBD basic info: battery=%s voltage=%.2fV current=%.2fA soc=%u%% residual=%.2fAh nominal=%.2fAh cycles=%u protection=0x%04x fet=0x%02x cells=%u ntc=%u version=%u",
                 battery_name,
                 voltage_v,
                 current_a,
                 soc_percent,
                 residual_ah,
                 nominal_ah,
                 cycles,
                 protection,
                 fet_status,
                 cell_count,
                 ntc_count,
                 version);
    }
}

bool parse_raw_adv_summary(const uint8_t *data, uint8_t length, AdvSummary *summary)
{
    if (summary == nullptr) {
        return false;
    }

    *summary = AdvSummary {};
    bool complete = true;
    uint8_t offset = 0;

    while (offset < length) {
        const uint8_t field_length = data[offset];
        if (field_length == 0) {
            break;
        }
        if (field_length == 1 || field_length > length - offset - 1) {
            complete = false;
            break;
        }

        const uint8_t type = data[offset + 1];
        const uint8_t *field_data = data + offset + 2;
        const uint8_t field_data_len = field_length - 1;

        switch (type) {
        case BLE_HS_ADV_TYPE_INCOMP_UUIDS16:
        case BLE_HS_ADV_TYPE_COMP_UUIDS16:
            summary->uuids16 = field_data;
            summary->uuids16_len = field_data_len;
            if (uuid16_field_has_service(field_data, field_data_len, kJbdServiceUuid)) {
                summary->has_jbd_service = true;
            }
            break;

        case BLE_HS_ADV_TYPE_SOL_UUIDS16:
            summary->sol_uuids16 = field_data;
            summary->sol_uuids16_len = field_data_len;
            if (uuid16_field_has_service(field_data, field_data_len, kJbdServiceUuid)) {
                summary->has_jbd_service = true;
            }
            break;

        case BLE_HS_ADV_TYPE_SVC_DATA_UUID16:
            summary->svc_data_uuid16 = field_data;
            summary->svc_data_uuid16_len = field_data_len;
            if (field_data_len >= 2 && uuid16_from_le(field_data) == kJbdServiceUuid) {
                summary->has_jbd_service = true;
            }
            break;

        case BLE_HS_ADV_TYPE_INCOMP_NAME:
        case BLE_HS_ADV_TYPE_COMP_NAME:
            summary->name = field_data;
            summary->name_len = field_data_len;
            summary->name_complete = type == BLE_HS_ADV_TYPE_COMP_NAME;
            break;

        case BLE_HS_ADV_TYPE_MFG_DATA:
            summary->mfg_data = field_data;
            summary->mfg_data_len = field_data_len;
            break;

        default:
            break;
        }

        offset += field_length + 1;
    }

    return complete;
}

const char *addr_type_name(uint8_t type)
{
    switch (type) {
    case BLE_ADDR_PUBLIC:
        return "public";
    case BLE_ADDR_RANDOM:
        return "random";
    case BLE_ADDR_PUBLIC_ID:
        return "public-id";
    case BLE_ADDR_RANDOM_ID:
        return "random-id";
    default:
        return "unknown";
    }
}

const char *event_type_name(uint8_t event_type)
{
    switch (event_type) {
    case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:
        return "adv";
    case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:
        return "direct";
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:
        return "scan";
    case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND:
        return "nonconn";
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP:
        return "scan_rsp";
    default:
        return "unknown";
    }
}

bool same_addr(const ble_addr_t &a, const ble_addr_t &b)
{
    return a.type == b.type && memcmp(a.val, b.val, sizeof(a.val)) == 0;
}

bool is_graphic_ascii(uint8_t ch)
{
    return isgraph(ch) != 0 && ch <= '~';
}

bool copy_trimmed_adv_name(const uint8_t *name, uint8_t name_len, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return false;
    }

    out[0] = '\0';
    if (name == nullptr || name_len == 0) {
        return false;
    }

    size_t start = 0;
    size_t end = name_len;
    while (start < end && !is_graphic_ascii(name[start])) {
        ++start;
    }
    while (end > start && !is_graphic_ascii(name[end - 1])) {
        --end;
    }
    if (start == end) {
        return false;
    }

    size_t copied = 0;
    for (size_t i = start; i < end && copied < out_size - 1; ++i) {
        out[copied] = static_cast<char>(name[i]);
        ++copied;
    }
    out[copied] = '\0';

    return battery_store_valid_name(out);
}

bool mark_seen_report(const ble_addr_t &addr, uint8_t event_type)
{
    for (size_t i = 0; i < g_seen_report_count; ++i) {
        if (same_addr(g_seen_reports[i].addr, addr) && g_seen_reports[i].event_type == event_type) {
            return false;
        }
    }

    if (g_seen_report_count < kSeenDevicesMax) {
        g_seen_reports[g_seen_report_count].addr = addr;
        g_seen_reports[g_seen_report_count].event_type = event_type;
        ++g_seen_report_count;
    }

    return true;
}

bool mark_candidate(const ble_addr_t &addr, int8_t rssi, const uint8_t *name, uint8_t name_len)
{
    char trimmed_name[kBatteryNameMax + 1] = {};
    const bool has_name = copy_trimmed_adv_name(name, name_len, trimmed_name, sizeof(trimmed_name));

    for (size_t i = 0; i < g_candidate_count; ++i) {
        if (same_addr(g_candidates[i].addr, addr)) {
            g_candidates[i].rssi = rssi;
            if (has_name) {
                strlcpy(g_candidates[i].name, trimmed_name, sizeof(g_candidates[i].name));
            }
            return false;
        }
    }

    if (g_candidate_count < kSeenDevicesMax) {
        g_candidates[g_candidate_count].addr = addr;
        g_candidates[g_candidate_count].rssi = rssi;
        if (has_name) {
            strlcpy(g_candidates[g_candidate_count].name,
                    trimmed_name,
                    sizeof(g_candidates[g_candidate_count].name));
        }
        ++g_candidate_count;
    }

    return true;
}

void print_bytes_hex(const uint8_t *bytes, uint8_t length)
{
    for (uint8_t i = 0; i < length; ++i) {
        printf("%02x", bytes[i]);
    }
}

void print_bytes_hex_size(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        printf("%02x", bytes[i]);
    }
}

void print_uuid16_list(const uint8_t *bytes, uint8_t length)
{
    for (uint8_t i = 0; i + 1 < length; i += 2) {
        if (i > 0) {
            printf(",");
        }
        printf("0x%04x", uuid16_from_le(bytes + i));
    }
}

void print_summary_fields(const AdvSummary &summary)
{
    if (summary.name != nullptr && summary.name_len > 0) {
        printf(" name=\"%.*s\"", summary.name_len, reinterpret_cast<const char *>(summary.name));
    }
    if (summary.uuids16 != nullptr && summary.uuids16_len > 0) {
        printf(" uuids16=");
        print_uuid16_list(summary.uuids16, summary.uuids16_len);
    }
    if (summary.sol_uuids16 != nullptr && summary.sol_uuids16_len > 0) {
        printf(" sol_uuids16=");
        print_uuid16_list(summary.sol_uuids16, summary.sol_uuids16_len);
    }
    if (summary.svc_data_uuid16 != nullptr && summary.svc_data_uuid16_len > 0) {
        printf(" svc_data_uuid16=");
        print_bytes_hex(summary.svc_data_uuid16, summary.svc_data_uuid16_len);
        if (summary.svc_data_uuid16_len >= 2) {
            printf("(uuid=0x%04x)", uuid16_from_le(summary.svc_data_uuid16));
        }
    }
    if (summary.mfg_data != nullptr && summary.mfg_data_len > 0) {
        printf(" mfg_data=");
        print_bytes_hex(summary.mfg_data, summary.mfg_data_len);
    }
}

void print_jbd_candidate(const ble_gap_disc_desc &disc, const ble_hs_adv_fields &fields)
{
    if (!g_verbose_battery_scanning) {
        return;
    }

    char addr[18] = {};
    format_addr(disc.addr, addr, sizeof(addr));

    printf("JBD candidate: addr=%s type=%s pdu=%s rssi=%d",
           addr,
           addr_type_name(disc.addr.type),
           event_type_name(disc.event_type),
           disc.rssi);

    if (fields.name != nullptr && fields.name_len > 0) {
        printf(" name=\"%.*s\"", fields.name_len, reinterpret_cast<const char *>(fields.name));
    }
    if (fields.tx_pwr_lvl_is_present) {
        printf(" tx_power=%d", fields.tx_pwr_lvl);
    }
    if (fields.svc_data_uuid16 != nullptr && fields.svc_data_uuid16_len > 0) {
        printf(" svc_data_uuid16=");
        print_bytes_hex(fields.svc_data_uuid16, fields.svc_data_uuid16_len);
    }
    if (fields.mfg_data != nullptr && fields.mfg_data_len > 0) {
        printf(" mfg_data=");
        print_bytes_hex(fields.mfg_data, fields.mfg_data_len);
    }

    printf(" service=0x%04x characteristics=0x%04x,0x%04x\n",
           kJbdServiceUuid,
           kJbdWriteCharacteristicUuid,
           kJbdNotifyCharacteristicUuid);
}

void print_jbd_candidate_raw(const ble_gap_disc_desc &disc, const AdvSummary &summary)
{
    if (!g_verbose_battery_scanning) {
        return;
    }

    char addr[18] = {};
    format_addr(disc.addr, addr, sizeof(addr));

    printf("JBD candidate: addr=%s type=%s pdu=%s rssi=%d",
           addr,
           addr_type_name(disc.addr.type),
           event_type_name(disc.event_type),
           disc.rssi);

    print_summary_fields(summary);

    printf(" service=0x%04x characteristics=0x%04x,0x%04x\n",
           kJbdServiceUuid,
           kJbdWriteCharacteristicUuid,
           kJbdNotifyCharacteristicUuid);
}

void set_probe_failed(int status)
{
    g_probe.status = status;
    if (g_scanner_events != nullptr) {
        xEventGroupSetBits(g_scanner_events, kProbeFailedBit);
    }
}

int service_discovered(uint16_t conn_handle,
                       const ble_gatt_error *error,
                       const ble_gatt_svc *service,
                       void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (error->status == 0 && service != nullptr) {
        g_probe.service_start = service->start_handle;
        g_probe.service_end = service->end_handle;
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        xEventGroupSetBits(g_scanner_events, kProbeServiceDoneBit);
    } else {
        set_probe_failed(error->status);
    }
    return 0;
}

int characteristic_discovered(uint16_t conn_handle,
                              const ble_gatt_error *error,
                              const ble_gatt_chr *chr,
                              void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (error->status == 0 && chr != nullptr) {
        if (uuid_is16(&chr->uuid.u, kJbdNotifyCharacteristicUuid)) {
            g_probe.ff01_def_handle = chr->def_handle;
            g_probe.ff01_val_handle = chr->val_handle;
            g_probe.ff01_properties = chr->properties;
            g_probe.notify_def_handle = chr->def_handle;
            g_probe.notify_val_handle = chr->val_handle;
            g_probe.notify_properties = chr->properties;
        } else if (uuid_is16(&chr->uuid.u, kJbdWriteCharacteristicUuid)) {
            g_probe.ff02_def_handle = chr->def_handle;
            g_probe.ff02_val_handle = chr->val_handle;
            g_probe.ff02_properties = chr->properties;
            g_probe.write_def_handle = chr->def_handle;
            g_probe.write_val_handle = chr->val_handle;
            g_probe.write_properties = chr->properties;
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        xEventGroupSetBits(g_scanner_events, kProbeCharsDoneBit);
    } else {
        set_probe_failed(error->status);
    }
    return 0;
}

int descriptor_discovered(uint16_t conn_handle,
                          const ble_gatt_error *error,
                          uint16_t chr_val_handle,
                          const ble_gatt_dsc *dsc,
                          void *arg)
{
    (void)conn_handle;
    (void)chr_val_handle;
    (void)arg;

    if (error->status == 0 && dsc != nullptr) {
        if (chr_val_handle == g_probe.notify_val_handle &&
            uuid_is16(&dsc->uuid.u, BLE_GATT_DSC_CLT_CFG_UUID16)) {
            g_probe.cccd_handle = dsc->handle;
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        xEventGroupSetBits(g_scanner_events, kProbeDscDoneBit);
    } else {
        set_probe_failed(error->status);
    }
    return 0;
}

int write_complete(uint16_t conn_handle,
                   const ble_gatt_error *error,
                   ble_gatt_attr *attr,
                   void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;

    if (error->status == 0) {
        xEventGroupSetBits(g_scanner_events, kProbeWriteDoneBit);
    } else {
        set_probe_failed(error->status);
    }
    return 0;
}

void consume_jbd_notification(const ble_gap_event *event)
{
    if (event->notify_rx.om == nullptr) {
        return;
    }

    const uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
    if (len == 0) {
        return;
    }

    uint8_t chunk[64] = {};
    const uint16_t copy_len = len < sizeof(chunk) ? len : sizeof(chunk);
    const int rc = os_mbuf_copydata(event->notify_rx.om,
                                    0,
                                    copy_len,
                                    chunk);
    if (rc != 0) {
        set_probe_failed(rc);
        return;
    }

    if (event->notify_rx.attr_handle != g_probe.notify_val_handle) {
        return;
    }
    if (g_probe.rx_len + len > sizeof(g_probe.rx)) {
        g_probe.rx_len = 0;
        set_probe_failed(BLE_HS_EMSGSIZE);
        return;
    }
    if (len > copy_len) {
        const int rest_rc = os_mbuf_copydata(event->notify_rx.om,
                                             copy_len,
                                             static_cast<uint16_t>(len - copy_len),
                                             &g_probe.rx[g_probe.rx_len + copy_len]);
        if (rest_rc != 0) {
            set_probe_failed(rest_rc);
            return;
        }
    }
    memcpy(&g_probe.rx[g_probe.rx_len], chunk, copy_len);

    g_probe.rx_len += len;
    if (find_jbd_basic_packet(g_probe.rx,
                              g_probe.rx_len,
                              &g_probe.packet_offset,
                              &g_probe.packet_len)) {
        xEventGroupSetBits(g_scanner_events, kProbePacketDoneBit);
    }
}

int probe_gap_event(ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_probe.conn_handle = event->connect.conn_handle;
            xEventGroupSetBits(g_scanner_events, kProbeConnectedBit);
        } else {
            set_probe_failed(event->connect.status);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        g_probe.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        xEventGroupSetBits(g_scanner_events, kProbeDisconnectedBit);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        consume_jbd_notification(event);
        return 0;

    default:
        return 0;
    }
}

bool wait_for_probe_bits(EventBits_t bits, TickType_t timeout)
{
    const EventBits_t result = xEventGroupWaitBits(g_scanner_events,
                                                   bits | kProbeFailedBit | kProbeDisconnectedBit,
                                                   pdTRUE,
                                                   pdFALSE,
                                                   timeout);
    return (result & bits) != 0;
}

void disconnect_probe(void)
{
    if (g_probe.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    const int rc = ble_gap_terminate(g_probe.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) {
        ESP_LOGW(kTag, "failed to disconnect from battery: rc=%d", rc);
        g_probe.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        return;
    }

    (void)xEventGroupWaitBits(g_scanner_events,
                              kProbeDisconnectedBit,
                              pdTRUE,
                              pdFALSE,
                              pdMS_TO_TICKS(2000));
}

bool discover_probe_handles(void)
{
    ble_uuid16_t service_uuid = BLE_UUID16_INIT(kJbdServiceUuid);
    int rc = ble_gattc_disc_svc_by_uuid(g_probe.conn_handle,
                                        &service_uuid.u,
                                        service_discovered,
                                        nullptr);
    if (rc != 0 || !wait_for_probe_bits(kProbeServiceDoneBit, kInterrogationTimeoutTicks)) {
        ESP_LOGW(kTag, "failed to discover JBD service: rc=%d status=%d", rc, g_probe.status);
        return false;
    }
    if (g_probe.service_start == 0 || g_probe.service_end == 0) {
        ESP_LOGW(kTag, "JBD service not found");
        return false;
    }

    rc = ble_gattc_disc_all_chrs(g_probe.conn_handle,
                                 g_probe.service_start,
                                 g_probe.service_end,
                                 characteristic_discovered,
                                 nullptr);
    if (rc != 0 || !wait_for_probe_bits(kProbeCharsDoneBit, kInterrogationTimeoutTicks)) {
        ESP_LOGW(kTag, "failed to discover JBD characteristics: rc=%d status=%d", rc, g_probe.status);
        return false;
    }
    if (g_probe.notify_val_handle == 0 || g_probe.write_val_handle == 0) {
        ESP_LOGW(kTag,
                 "missing JBD characteristics: notify=0x%04x write=0x%04x",
                 g_probe.notify_val_handle,
                 g_probe.write_val_handle);
        return false;
    }

    if (g_probe.ff01_val_handle != 0 && char_can_notify(g_probe.ff01_properties)) {
        g_probe.notify_def_handle = g_probe.ff01_def_handle;
        g_probe.notify_val_handle = g_probe.ff01_val_handle;
        g_probe.notify_properties = g_probe.ff01_properties;
    } else if (g_probe.ff02_val_handle != 0 && char_can_notify(g_probe.ff02_properties)) {
        g_probe.notify_def_handle = g_probe.ff02_def_handle;
        g_probe.notify_val_handle = g_probe.ff02_val_handle;
        g_probe.notify_properties = g_probe.ff02_properties;
    }

    if (g_probe.ff02_val_handle != 0 && char_can_write(g_probe.ff02_properties)) {
        g_probe.write_def_handle = g_probe.ff02_def_handle;
        g_probe.write_val_handle = g_probe.ff02_val_handle;
        g_probe.write_properties = g_probe.ff02_properties;
    } else if (g_probe.ff01_val_handle != 0 && char_can_write(g_probe.ff01_properties)) {
        g_probe.write_def_handle = g_probe.ff01_def_handle;
        g_probe.write_val_handle = g_probe.ff01_val_handle;
        g_probe.write_properties = g_probe.ff01_properties;
    }

    if (!char_can_notify(g_probe.notify_properties) || !char_can_write(g_probe.write_properties)) {
        ESP_LOGW(kTag,
                 "JBD characteristics lack expected properties: notify=0x%04x props=0x%02x write=0x%04x props=0x%02x",
                 g_probe.notify_val_handle,
                 g_probe.notify_properties,
                 g_probe.write_val_handle,
                 g_probe.write_properties);
        return false;
    }

    uint16_t descriptor_end = g_probe.service_end;
    if (g_probe.write_def_handle > g_probe.notify_val_handle) {
        descriptor_end = static_cast<uint16_t>(g_probe.write_def_handle - 1);
    }

    rc = ble_gattc_disc_all_dscs(g_probe.conn_handle,
                                 g_probe.notify_val_handle,
                                 descriptor_end,
                                 descriptor_discovered,
                                 nullptr);
    if (rc != 0 || !wait_for_probe_bits(kProbeDscDoneBit, kInterrogationTimeoutTicks)) {
        ESP_LOGW(kTag, "failed to discover JBD notify descriptors: rc=%d status=%d", rc, g_probe.status);
        return false;
    }
    if (g_probe.cccd_handle == 0) {
        ESP_LOGW(kTag, "JBD notify CCCD not found");
        return false;
    }

    return true;
}

bool subscribe_probe_notifications(void)
{
    const uint8_t subscribe_notify[2] = {0x01, 0x00};

    const int rc = ble_gattc_write_flat(g_probe.conn_handle,
                                        g_probe.cccd_handle,
                                        subscribe_notify,
                                        sizeof(subscribe_notify),
                                        write_complete,
                                        nullptr);
    if (rc != 0 || !wait_for_probe_bits(kProbeWriteDoneBit, kInterrogationTimeoutTicks)) {
        ESP_LOGW(kTag, "failed to subscribe to JBD notifications: rc=%d status=%d", rc, g_probe.status);
        return false;
    }

    vTaskDelay(kJbdCommandRetryTicks);
    return true;
}

bool send_basic_info_request(void)
{
    static constexpr uint8_t request[] = {0xdd, 0xa5, 0x03, 0x00, 0xff, 0xfd, 0x77};

    if ((g_probe.write_properties & BLE_GATT_CHR_F_WRITE_NO_RSP) != 0) {
        const int rc = ble_gattc_write_no_rsp_flat(g_probe.conn_handle,
                                                  g_probe.write_val_handle,
                                                  request,
                                                  sizeof(request));
        if (rc != 0) {
            ESP_LOGW(kTag, "failed to send JBD basic info request without response: rc=%d", rc);
            return false;
        }
        return true;
    }

    const int rc = ble_gattc_write_flat(g_probe.conn_handle,
                                        g_probe.write_val_handle,
                                        request,
                                        sizeof(request),
                                        write_complete,
                                        nullptr);
    if (rc != 0 || !wait_for_probe_bits(kProbeWriteDoneBit, kInterrogationTimeoutTicks)) {
        ESP_LOGW(kTag, "failed to send JBD basic info request: rc=%d status=%d", rc, g_probe.status);
        return false;
    }

    return true;
}

void probe_battery(const BatteryCandidate &candidate)
{
    char addr[18] = {};
    format_addr(candidate.addr, addr, sizeof(addr));
    const char *battery_name = candidate.name[0] != '\0' ? candidate.name : addr;

    g_probe = ProbeState {};
    g_probe.addr = candidate.addr;
    xEventGroupClearBits(g_scanner_events,
                         kProbeConnectedBit | kProbeDisconnectedBit | kProbeServiceDoneBit |
                             kProbeCharsDoneBit | kProbeDscDoneBit | kProbeSubscribeDoneBit |
                             kProbeWriteDoneBit | kProbePacketDoneBit | kProbeFailedBit);

    ble_gap_conn_params params = {};
    params.scan_itvl = 0x0010;
    params.scan_window = 0x0010;
    params.itvl_min = 24;
    params.itvl_max = 40;
    params.latency = 0;
    params.supervision_timeout = 400;

    int rc = ble_gap_connect(ble_manager_own_addr_type(),
                             &candidate.addr,
                             5000,
                             &params,
                             probe_gap_event,
                             nullptr);
    if (rc != 0) {
        ESP_LOGW(kTag, "failed to start JBD battery connection: addr=%s rc=%d", addr, rc);
        return;
    }

    if (!wait_for_probe_bits(kProbeConnectedBit, kInterrogationTimeoutTicks)) {
        ESP_LOGW(kTag, "JBD battery connection timed out: addr=%s status=%d", addr, g_probe.status);
        disconnect_probe();
        return;
    }

    if (!discover_probe_handles() || !subscribe_probe_notifications() || !send_basic_info_request()) {
        disconnect_probe();
        return;
    }

    bool got_packet = wait_for_probe_bits(kProbePacketDoneBit, kJbdCommandRetryTicks);
    if (!got_packet) {
        if (!send_basic_info_request()) {
            disconnect_probe();
            return;
        }
        got_packet = wait_for_probe_bits(kProbePacketDoneBit, kInterrogationTimeoutTicks);
    }

    if (got_packet) {
        if (g_verbose_battery_scanning) {
            printf("JBD basic info packet: addr=%s data=", addr);
            print_bytes_hex_size(&g_probe.rx[g_probe.packet_offset], g_probe.packet_len);
            printf("\n");
        }
        log_jbd_basic_info(battery_name, &g_probe.rx[g_probe.packet_offset], g_probe.packet_len);
    } else {
        ESP_LOGW(kTag, "no valid JBD basic info packet before timeout: addr=%s", addr);
    }

    disconnect_probe();
}

int scan_event(ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        ++g_scan_stats.reports;
        if (event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
            ++g_scan_stats.scan_response_reports;
        } else {
            ++g_scan_stats.adv_reports;
        }

        AdvSummary summary = {};
        const bool raw_complete =
            parse_raw_adv_summary(event->disc.data, event->disc.length_data, &summary);
        if (!raw_complete) {
            ++g_scan_stats.raw_malformed;
        }

        ble_hs_adv_fields fields = {};
        const int rc = ble_hs_adv_parse_fields(&fields,
                                               event->disc.data,
                                               event->disc.length_data);
        if (rc != 0) {
            ++g_scan_stats.parse_errors;
            if (summary.has_jbd_service) {
                mark_candidate(event->disc.addr, event->disc.rssi, summary.name, summary.name_len);
                ++g_scan_stats.jbd_reports;
                if (mark_seen_report(event->disc.addr, event->disc.event_type)) {
                    ++g_scan_stats.printed_reports;
                    print_jbd_candidate_raw(event->disc, summary);
                } else {
                    ++g_scan_stats.duplicate_jbd_reports;
                }
            }
            return 0;
        }

        if (has_jbd_service(fields) || summary.has_jbd_service) {
            mark_candidate(event->disc.addr, event->disc.rssi, fields.name, fields.name_len);
            ++g_scan_stats.jbd_reports;
            if (mark_seen_report(event->disc.addr, event->disc.event_type)) {
                ++g_scan_stats.printed_reports;
                print_jbd_candidate(event->disc, fields);
            } else {
                ++g_scan_stats.duplicate_jbd_reports;
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (g_verbose_battery_scanning) {
            ESP_LOGI(kTag,
                     "BLE battery scan complete: reason=%d reports=%" PRIu32
                     " adv=%" PRIu32 " scan_rsp=%" PRIu32 " parse_errors=%" PRIu32
                     " raw_malformed=%" PRIu32 " jbd_reports=%" PRIu32
                     " printed=%" PRIu32 " duplicate_jbd=%" PRIu32,
                     event->disc_complete.reason,
                     g_scan_stats.reports,
                     g_scan_stats.adv_reports,
                     g_scan_stats.scan_response_reports,
                     g_scan_stats.parse_errors,
                     g_scan_stats.raw_malformed,
                     g_scan_stats.jbd_reports,
                     g_scan_stats.printed_reports,
                     g_scan_stats.duplicate_jbd_reports);
        }
        if (g_scanner_events != nullptr) {
            xEventGroupSetBits(g_scanner_events, kScanDoneBit);
        }
        return 0;

    default:
        return 0;
    }
}

esp_err_t run_scan(void)
{
    ble_gap_disc_params params = {};
    params.itvl = 500;
    params.window = 250;
    params.filter_policy = 0;
    params.limited = 0;
    params.passive = 0;
    params.filter_duplicates = 1;
    params.disable_observer_mode = 0;

    if (g_verbose_battery_scanning) {
        ESP_LOGI(kTag,
                 "BLE battery scan starting: service=0x%04x duration=%" PRIu32
                 "ms active=yes duplicates=data-device",
                 kJbdServiceUuid,
                 kScanDurationMs);
    }

    g_seen_report_count = 0;
    g_candidate_count = 0;
    g_scan_stats = ScanStats {};
    xEventGroupClearBits(g_scanner_events, kScanDoneBit);

    const int rc =
        ble_gap_disc(ble_manager_own_addr_type(), kScanDurationMs, &params, scan_event, nullptr);
    if (rc != 0) {
        ESP_LOGW(kTag, "failed to start BLE battery scan: rc=%d", rc);
        return ESP_FAIL;
    }

    const EventBits_t bits = xEventGroupWaitBits(g_scanner_events,
                                                 kScanDoneBit,
                                                 pdTRUE,
                                                 pdFALSE,
                                                 pdMS_TO_TICKS(kScanDurationMs + 1000));
    if ((bits & kScanDoneBit) == 0) {
        ESP_LOGW(kTag, "BLE battery scan timed out locally");
        (void)ble_gap_disc_cancel();
    }

    for (size_t i = 0; i < g_candidate_count; ++i) {
        probe_battery(g_candidates[i]);
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    return ESP_OK;
}

void battery_scanner_task(void *arg)
{
    (void)arg;

    while (ble_manager_wait_until_synced(kSyncWaitTicks) != ESP_OK) {
        ESP_LOGW(kTag, "waiting for BLE host sync before scanning");
    }

    while (true) {
        (void)run_scan();
        vTaskDelay(kScanPeriodTicks);
    }
}

}  // namespace

esp_err_t battery_scanner_start(void)
{
    if (g_scanner_task != nullptr) {
        return ESP_OK;
    }

    if (g_scanner_events == nullptr) {
        g_scanner_events = xEventGroupCreate();
        if (g_scanner_events == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t created = xTaskCreate(battery_scanner_task,
                                     "battery_scanner",
                                     6144,
                                     nullptr,
                                     4,
                                     &g_scanner_task);
    if (created != pdPASS) {
        g_scanner_task = nullptr;
        return ESP_ERR_NO_MEM;
    }

    if (g_verbose_battery_scanning) {
        ESP_LOGI(kTag, "battery scanner task started");
    }
    return ESP_OK;
}

void battery_scanner_set_verbose(bool enabled)
{
    g_verbose_battery_scanning = enabled;
}

bool battery_scanner_verbose_enabled(void)
{
    return g_verbose_battery_scanning;
}
