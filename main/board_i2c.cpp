#include "board_i2c.hpp"

#include "esp_check.h"
#include "esp_log.h"

namespace {

constexpr const char *kTag = "board_i2c";
i2c_master_bus_handle_t g_bus = nullptr;

}  // namespace

esp_err_t board_i2c_start(const I2cHardwareConfig &config)
{
    if (g_bus != nullptr) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(config.present, ESP_ERR_NOT_SUPPORTED, kTag, "board has no I2C bus");

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = static_cast<i2c_port_num_t>(config.port);
    bus_config.sda_io_num = static_cast<gpio_num_t>(config.sda_gpio);
    bus_config.scl_io_num = static_cast<gpio_num_t>(config.scl_gpio);
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = false;

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &g_bus),
                        kTag,
                        "failed to initialize I2C bus");
    ESP_LOGI(kTag,
             "started port=%d sda=%d scl=%d",
             config.port,
             config.sda_gpio,
             config.scl_gpio);
    return ESP_OK;
}

esp_err_t board_i2c_add_device(uint8_t address,
                               uint32_t frequency_hz,
                               i2c_master_dev_handle_t *device)
{
    ESP_RETURN_ON_FALSE(g_bus != nullptr && device != nullptr,
                        ESP_ERR_INVALID_STATE,
                        kTag,
                        "I2C bus is not available");

    i2c_device_config_t device_config = {};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = address;
    device_config.scl_speed_hz = frequency_hz;
    return i2c_master_bus_add_device(g_bus, &device_config, device);
}

bool board_i2c_started(void)
{
    return g_bus != nullptr;
}
