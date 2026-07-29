#include "input_manager.hpp"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

namespace {

constexpr const char *kTag = "input_manager";

InputManagerStatus g_status = {};
DigitalInputHardwareConfig g_hardware = {};

}  // namespace

esp_err_t input_manager_start(const BoardConfig &board)
{
    g_status = {};
    g_hardware = board.digital_input;
    g_status.present = g_hardware.kind == DigitalInputBackendKind::Gpio;
    g_status.count = g_hardware.count;
    if (!g_status.present) {
        g_status.initialization_result = ESP_ERR_NOT_SUPPORTED;
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint64_t pin_mask = 0;
    for (uint8_t i = 0; i < g_hardware.count; ++i) {
        pin_mask |= 1ULL << static_cast<unsigned>(g_hardware.channels[i]);
    }

    gpio_config_t config = {};
    config.pin_bit_mask = pin_mask;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en =
        g_hardware.pull == GpioPullKind::Up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    config.pull_down_en =
        g_hardware.pull == GpioPullKind::Down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    const esp_err_t err = gpio_config(&config);
    g_status.initialization_result = err;
    g_status.initialized = err == ESP_OK;
    if (err == ESP_OK) {
        ESP_LOGI(kTag,
                 "%u GPIO inputs available, active level %u, pull %s",
                 g_hardware.count,
                 g_hardware.active_level,
                 gpio_pull_name(g_hardware.pull));
    } else {
        ESP_LOGE(kTag, "failed to initialize digital inputs: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t input_manager_get_status(InputManagerStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *status = g_status;
    return ESP_OK;
}

uint8_t input_manager_count(void)
{
    return g_status.initialized ? g_status.count : 0;
}

esp_err_t input_manager_query(uint8_t input, InputStatus *status)
{
    ESP_RETURN_ON_FALSE(status != nullptr,
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "missing digital input result");
    ESP_RETURN_ON_FALSE(g_status.initialized,
                        g_status.present ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_SUPPORTED,
                        kTag,
                        "digital inputs are unavailable");
    ESP_RETURN_ON_FALSE(input >= 1 && input <= g_hardware.count,
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "digital input number is out of range");

    const int gpio = g_hardware.channels[input - 1];
    const int level = gpio_get_level(static_cast<gpio_num_t>(gpio));
    *status = {
        .input = input,
        .backend = g_hardware.kind,
        .gpio_pin = gpio,
        .active_level = g_hardware.active_level,
        .level = level,
        .on = level == g_hardware.active_level,
    };
    return ESP_OK;
}
