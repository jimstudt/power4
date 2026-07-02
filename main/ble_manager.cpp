#include "ble_manager.hpp"

#include <string.h>

#include "config_gatt.hpp"
#include "esp_log.h"
#include "relay_gatt.hpp"

extern "C" {
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

namespace {

constexpr const char *kTag = "ble_manager";
constexpr const char *kDeviceName = "power4";

bool g_ble_started = false;
uint8_t g_own_addr_type = 0;

void ble_advertise(void);

int ble_gap_event(ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(kTag, "BLE client connected");
        } else {
            ESP_LOGW(kTag, "BLE connect failed: status=%d", event->connect.status);
            ble_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(kTag, "BLE client disconnected: reason=%d", event->disconnect.reason);
        ble_advertise();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(kTag, "BLE advertising complete: reason=%d", event->adv_complete.reason);
        ble_advertise();
        break;

    default:
        break;
    }

    return 0;
}

void ble_advertise(void)
{
    ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    fields.uuids128 = relay_gatt_service_uuid();
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(kTag, "failed to set BLE advertising fields: rc=%d", rc);
        return;
    }

    ble_hs_adv_fields scan_response = {};
    const char *name = ble_svc_gap_device_name();
    scan_response.name = reinterpret_cast<const uint8_t *>(name);
    scan_response.name_len = strlen(name);
    scan_response.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&scan_response);
    if (rc != 0) {
        ESP_LOGE(kTag, "failed to set BLE scan response fields: rc=%d", rc);
        return;
    }

    ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(g_own_addr_type,
                           nullptr,
                           BLE_HS_FOREVER,
                           &adv_params,
                           ble_gap_event,
                           nullptr);
    if (rc != 0) {
        ESP_LOGE(kTag, "failed to start BLE advertising: rc=%d", rc);
        return;
    }

    ESP_LOGI(kTag, "BLE advertising as %s", name);
}

void ble_on_reset(int reason)
{
    ESP_LOGW(kTag, "NimBLE host reset: reason=%d", reason);
}

void ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(kTag, "failed to ensure BLE address: rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(kTag, "failed to infer BLE address type: rc=%d", rc);
        return;
    }

    ESP_LOGI(kTag, "NimBLE host synchronized");
    ble_advertise();
}

void ble_host_task(void *arg)
{
    (void)arg;

    ESP_LOGI(kTag, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
    ESP_LOGW(kTag, "NimBLE host task stopped");
}

}  // namespace

esp_err_t ble_manager_start(void)
{
    if (g_ble_started) {
        return ESP_OK;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "failed to initialize NimBLE: %s", esp_err_to_name(err));
        return err;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_svc_gap_device_name_set(kDeviceName);
    if (rc != 0) {
        ESP_LOGE(kTag, "failed to set BLE device name: rc=%d", rc);
        return ESP_FAIL;
    }

    err = relay_gatt_register();
    if (err != ESP_OK) {
        return err;
    }

    err = config_gatt_register();
    if (err != ESP_OK) {
        return err;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;

    nimble_port_freertos_init(ble_host_task);
    g_ble_started = true;
    ESP_LOGI(kTag, "NimBLE started");
    return ESP_OK;
}
