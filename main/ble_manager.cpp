#include "ble_manager.hpp"

#include "esp_log.h"

extern "C" {
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
}

namespace {

constexpr const char *kTag = "ble_manager";

bool g_ble_started = false;

void ble_on_reset(int reason)
{
    ESP_LOGW(kTag, "NimBLE host reset: reason=%d", reason);
}

void ble_on_sync(void)
{
    ESP_LOGI(kTag, "NimBLE host synchronized");
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

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;

    nimble_port_freertos_init(ble_host_task);
    g_ble_started = true;
    ESP_LOGI(kTag, "NimBLE started");
    return ESP_OK;
}
