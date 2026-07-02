#include "relay_gatt.hpp"

#include <string.h>

#include "esp_log.h"
#include "relay_manager.hpp"
#include "sdkconfig.h"

extern "C" {
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
}

namespace {

constexpr const char *kTag = "relay_gatt";
constexpr uint16_t kServiceUuidSuffix = 0x1000;
constexpr uint16_t kFirstRelayUuidSuffix = 0x1001;

struct RelayCharacteristicArg {
    uint8_t relay = 0;
};

ble_uuid128_t g_service_uuid = {};
ble_uuid128_t g_relay_uuids[CONFIG_POWER4_RELAY_COUNT] = {};
RelayCharacteristicArg g_relay_args[CONFIG_POWER4_RELAY_COUNT] = {};
ble_gatt_chr_def g_characteristics[CONFIG_POWER4_RELAY_COUNT + 1] = {};
ble_gatt_svc_def g_services[2] = {};
bool g_registered = false;

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

int relay_state_access(uint16_t conn_handle,
                       uint16_t attr_handle,
                       ble_gatt_access_ctxt *ctxt,
                       void *arg)
{
    (void)conn_handle;
    (void)attr_handle;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR || arg == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const RelayCharacteristicArg *relay_arg = static_cast<const RelayCharacteristicArg *>(arg);
    RelayStatus status = {};
    const esp_err_t err = relay_manager_query(relay_arg->relay, &status);
    if (err != ESP_OK) {
        ESP_LOGW(kTag,
                 "failed to read relay %u for GATT client: %s",
                 relay_arg->relay,
                 esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint8_t value = status.output_on ? 1 : 0;
    return os_mbuf_append(ctxt->om, &value, sizeof(value)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

}  // namespace

esp_err_t relay_gatt_register(void)
{
    if (g_registered) {
        return ESP_OK;
    }

    memset(g_characteristics, 0, sizeof(g_characteristics));
    memset(g_services, 0, sizeof(g_services));

    g_service_uuid = make_power4_uuid(kServiceUuidSuffix);

    const uint8_t relay_count = relay_manager_count();
    for (uint8_t relay = 1; relay <= relay_count; ++relay) {
        const uint8_t index = relay - 1;
        g_relay_uuids[index] =
            make_power4_uuid(static_cast<uint16_t>(kFirstRelayUuidSuffix + index));
        g_relay_args[index].relay = relay;

        g_characteristics[index].uuid = &g_relay_uuids[index].u;
        g_characteristics[index].access_cb = relay_state_access;
        g_characteristics[index].arg = &g_relay_args[index];
        g_characteristics[index].flags = BLE_GATT_CHR_F_READ;
    }

    g_services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_services[0].uuid = &g_service_uuid.u;
    g_services[0].characteristics = g_characteristics;

    int rc = ble_gatts_count_cfg(g_services);
    if (rc != 0) {
        ESP_LOGE(kTag, "failed to count relay GATT service: rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(g_services);
    if (rc != 0) {
        ESP_LOGE(kTag, "failed to add relay GATT service: rc=%d", rc);
        return ESP_FAIL;
    }

    g_registered = true;
    ESP_LOGI(kTag, "registered %u relay binary sensor characteristics", relay_count);
    return ESP_OK;
}

const ble_uuid128_t *relay_gatt_service_uuid(void)
{
    return &g_service_uuid;
}
