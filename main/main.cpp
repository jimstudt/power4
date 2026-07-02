#include "ble_manager.hpp"
#include "console.hpp"
#include "policy_task.hpp"
#include "policy_storage.hpp"
#include "relay_manager.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char *kTag = "power4";

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "starting power4");
    ESP_ERROR_CHECK(policy_storage_init());
    ESP_ERROR_CHECK(relay_manager_start());
    ESP_ERROR_CHECK(ble_manager_start());
    ESP_ERROR_CHECK(policy_task_start());
    ESP_ERROR_CHECK(power4_console_start());

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
