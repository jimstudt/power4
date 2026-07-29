#include "ble_manager.hpp"
#include "battery_bank.hpp"
#include "battery_scanner.hpp"
#include "battery_store.hpp"
#include "board_config.hpp"
#include "board_i2c.hpp"
#include "console.hpp"
#include "ethernet_manager.hpp"
#include "input_manager.hpp"
#include "log_buffer.hpp"
#include "network_console.hpp"
#include "policy_task.hpp"
#include "policy_storage.hpp"
#include "relay_manager.hpp"
#include "rtc_manager.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char *kTag = "power4";

}  // namespace

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(log_buffer_init());
    ESP_LOGI(kTag, "starting power4");
    ESP_ERROR_CHECK(policy_storage_init());
    ESP_ERROR_CHECK(network_console_init());
    ESP_ERROR_CHECK(battery_store_init());
    ESP_ERROR_CHECK(battery_bank_init());

    const esp_err_t board_err = board_config_init();
    const BoardConfig *board = board_config_get();
    bool relay_control_ready = false;
    bool ethernet_ready = false;
    if (board_err != ESP_OK || board == nullptr) {
        ESP_LOGE(kTag,
                 "board configuration unavailable; relay policy disabled: %s",
                 board_config_error());
    } else {
        if (board->i2c.present) {
            const esp_err_t i2c_err = board_i2c_start(board->i2c);
            if (i2c_err != ESP_OK) {
                ESP_LOGE(kTag, "board I2C unavailable: %s", esp_err_to_name(i2c_err));
            }
        }

        // Establish the conservative relay state before initializing optional
        // peripherals, which may take appreciable time or fail independently.
        const esp_err_t relay_err = relay_manager_start(*board);
        if (relay_err == ESP_OK) {
            relay_control_ready = true;
        } else {
            ESP_LOGE(kTag,
                     "relay hardware unavailable; relay policy disabled: %s",
                     esp_err_to_name(relay_err));
        }

        if (board->digital_input.kind != DigitalInputBackendKind::None) {
            const esp_err_t input_err = input_manager_start(*board);
            if (input_err != ESP_OK) {
                ESP_LOGE(kTag, "digital inputs unavailable: %s", esp_err_to_name(input_err));
            }
        }

        if (board->rtc.kind != RtcHardwareKind::None) {
            const esp_err_t rtc_err = rtc_manager_start(*board);
            if (rtc_err != ESP_OK) {
                ESP_LOGE(kTag, "RTC unavailable: %s", esp_err_to_name(rtc_err));
            }
        }

        if (board->ethernet.kind != EthernetHardwareKind::None) {
            const esp_err_t ethernet_err = ethernet_manager_start(*board);
            if (ethernet_err != ESP_OK) {
                ESP_LOGE(kTag, "Ethernet unavailable: %s", esp_err_to_name(ethernet_err));
            } else {
                ethernet_ready = true;
            }
        }
    }

    if (relay_control_ready) {
        ESP_ERROR_CHECK(ble_manager_start());
        ESP_ERROR_CHECK(battery_scanner_start());
        ESP_ERROR_CHECK(policy_task_start());
    }
    ESP_ERROR_CHECK(power4_console_start());
    if (ethernet_ready) {
        const esp_err_t network_console_err = network_console_start();
        if (network_console_err != ESP_OK) {
            ESP_LOGE(kTag,
                     "TCP console unavailable: %s",
                     esp_err_to_name(network_console_err));
        }
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
