#include "config_gatt.hpp"

#include <string.h>

#include "config_flags.hpp"
#include "esp_log.h"

extern "C" {
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
}

namespace {

constexpr const char *kTag = "config_gatt";
constexpr uint16_t kServiceUuidSuffix = 0x2000;
constexpr uint16_t kListUuidSuffix = 0x2001;
constexpr uint16_t kSetUuidSuffix = 0x2002;
constexpr uint16_t kUnsetUuidSuffix = 0x2003;
constexpr size_t kFlagListTextMaxBytes = kConfigFlagListMax * kConfigFlagNameMaxBytes;

enum class Characteristic : uint8_t {
    List,
    Set,
    Unset,
};

ble_uuid128_t g_service_uuid = {};
ble_uuid128_t g_list_uuid = {};
ble_uuid128_t g_set_uuid = {};
ble_uuid128_t g_unset_uuid = {};
ble_gatt_chr_def g_characteristics[4] = {};
ble_gatt_svc_def g_services[2] = {};
bool g_registered = false;
const Characteristic g_list_characteristic = Characteristic::List;
const Characteristic g_set_characteristic = Characteristic::Set;
const Characteristic g_unset_characteristic = Characteristic::Unset;

ble_uuid128_t make_power4_uuid(uint16_t suffix)
{
    ble_uuid128_t uuid = BLE_UUID128_INIT(0x00,
                                          0x10,
                                          0x0c,
                                          0x7e,
                                          0x4a,
                                          0x0f,
                                          0x2b,
                                          0x8f,
                                          0x7d,
                                          0x4a,
                                          0x10,
                                          0x9a,
                                          0xf0,
                                          0xd5,
                                          0xc7,
                                          0x79);
    uuid.value[0] = static_cast<uint8_t>(suffix & 0xff);
    uuid.value[1] = static_cast<uint8_t>((suffix >> 8) & 0xff);
    return uuid;
}

int read_flag_list(ble_gatt_access_ctxt *ctxt)
{
    ConfigFlagList flags = {};
    esp_err_t err = config_flags_list(&flags);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "failed to list config flags for GATT client: %s", esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    char value[kFlagListTextMaxBytes] = {};
    size_t offset = 0;

    for (size_t i = 0; i < flags.count; ++i) {
        const char *name = flags.names[i];
        const size_t name_length = strlen(name);
        if (offset + name_length + 1 > sizeof(value)) {
            ESP_LOGW(kTag, "config flag list exceeded GATT read buffer");
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        memcpy(&value[offset], name, name_length);
        offset += name_length;
        value[offset] = '\n';
        ++offset;
    }

    return os_mbuf_append(ctxt->om, value, offset) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int read_written_name(ble_gatt_access_ctxt *ctxt, char *name, size_t name_size)
{
    const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length == 0 || length >= name_size) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint16_t copied = 0;
    const int rc = ble_hs_mbuf_to_flat(ctxt->om, name, name_size - 1, &copied);
    if (rc != 0 || copied != length) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    name[copied] = '\0';
    if (!config_flags_valid_name(name)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    return 0;
}

int write_flag(ble_gatt_access_ctxt *ctxt, bool set_flag)
{
    char name[kConfigFlagNameMaxBytes] = {};
    int result = read_written_name(ctxt, name, sizeof(name));
    if (result != 0) {
        return result;
    }

    const esp_err_t err = set_flag ? config_flags_set(name) : config_flags_unset(name);
    if (err != ESP_OK) {
        ESP_LOGW(kTag,
                 "failed to %s config flag '%s' for GATT client: %s",
                 set_flag ? "set" : "unset",
                 name,
                 esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    ESP_LOGI(kTag, "BLE %s config flag '%s'", set_flag ? "set" : "unset", name);
    return 0;
}

int config_access(uint16_t conn_handle,
                  uint16_t attr_handle,
                  ble_gatt_access_ctxt *ctxt,
                  void *arg)
{
    (void)conn_handle;
    (void)attr_handle;

    if (arg == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const Characteristic characteristic = *static_cast<const Characteristic *>(arg);

    switch (characteristic) {
    case Characteristic::List:
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        return read_flag_list(ctxt);

    case Characteristic::Set:
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        return write_flag(ctxt, true);

    case Characteristic::Unset:
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        return write_flag(ctxt, false);
    }

    return BLE_ATT_ERR_UNLIKELY;
}

}  // namespace

esp_err_t config_gatt_register(void)
{
    if (g_registered) {
        return ESP_OK;
    }

    memset(g_characteristics, 0, sizeof(g_characteristics));
    memset(g_services, 0, sizeof(g_services));

    g_service_uuid = make_power4_uuid(kServiceUuidSuffix);
    g_list_uuid = make_power4_uuid(kListUuidSuffix);
    g_set_uuid = make_power4_uuid(kSetUuidSuffix);
    g_unset_uuid = make_power4_uuid(kUnsetUuidSuffix);

    g_characteristics[0].uuid = &g_list_uuid.u;
    g_characteristics[0].access_cb = config_access;
    g_characteristics[0].arg = const_cast<Characteristic *>(&g_list_characteristic);
    g_characteristics[0].flags = BLE_GATT_CHR_F_READ;

    g_characteristics[1].uuid = &g_set_uuid.u;
    g_characteristics[1].access_cb = config_access;
    g_characteristics[1].arg = const_cast<Characteristic *>(&g_set_characteristic);
    g_characteristics[1].flags = BLE_GATT_CHR_F_WRITE;

    g_characteristics[2].uuid = &g_unset_uuid.u;
    g_characteristics[2].access_cb = config_access;
    g_characteristics[2].arg = const_cast<Characteristic *>(&g_unset_characteristic);
    g_characteristics[2].flags = BLE_GATT_CHR_F_WRITE;

    g_services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_services[0].uuid = &g_service_uuid.u;
    g_services[0].characteristics = g_characteristics;

    int rc = ble_gatts_count_cfg(g_services);
    if (rc != 0) {
        ESP_LOGE(kTag, "failed to count config GATT service: rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(g_services);
    if (rc != 0) {
        ESP_LOGE(kTag, "failed to add config GATT service: rc=%d", rc);
        return ESP_FAIL;
    }

    g_registered = true;
    ESP_LOGI(kTag, "registered config flag GATT service");
    return ESP_OK;
}
