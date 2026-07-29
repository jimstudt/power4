#include "board_config.hpp"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr const char *kTag = "board_config";
constexpr const char *kPartition = "board_config";
constexpr const char *kNamespace = "board";

BoardConfig g_config = {};
esp_err_t g_result = ESP_ERR_INVALID_STATE;
char g_error[160] = "board configuration has not been loaded";

void set_error(esp_err_t result, const char *message)
{
    g_result = result;
    strlcpy(g_error, message, sizeof(g_error));
    ESP_LOGE(kTag, "%s: %s", message, esp_err_to_name(result));
}

esp_err_t get_string(nvs_handle_t handle, const char *key, char *value, size_t capacity)
{
    size_t length = capacity;
    esp_err_t err = nvs_get_str(handle, key, value, &length);
    if (err == ESP_OK && (length == 0 || value[0] == '\0')) {
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

esp_err_t get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    return nvs_get_u8(handle, key, value);
}

esp_err_t get_u32_as_int(nvs_handle_t handle, const char *key, int *value)
{
    uint32_t stored = 0;
    ESP_RETURN_ON_ERROR(nvs_get_u32(handle, key, &stored), kTag, "missing %s", key);
    ESP_RETURN_ON_FALSE(stored <= INT32_MAX, ESP_ERR_INVALID_ARG, kTag, "%s is too large", key);
    *value = static_cast<int>(stored);
    return ESP_OK;
}

esp_err_t parse_channel_map(const char *text, uint8_t count, int *channels)
{
    const char *cursor = text;
    for (uint8_t i = 0; i < count; ++i) {
        while (isspace(static_cast<unsigned char>(*cursor))) {
            ++cursor;
        }
        errno = 0;
        char *end = nullptr;
        long value = strtol(cursor, &end, 10);
        ESP_RETURN_ON_FALSE(end != cursor && errno == 0 && value >= 0 && value <= INT32_MAX,
                            ESP_ERR_INVALID_ARG,
                            kTag,
                            "invalid channel map");
        channels[i] = static_cast<int>(value);
        cursor = end;
        while (isspace(static_cast<unsigned char>(*cursor))) {
            ++cursor;
        }
        if (i + 1 < count) {
            ESP_RETURN_ON_FALSE(*cursor == ',', ESP_ERR_INVALID_ARG, kTag, "short relay map");
            ++cursor;
        } else {
            ESP_RETURN_ON_FALSE(*cursor == '\0', ESP_ERR_INVALID_ARG, kTag, "long relay map");
        }
    }
    return ESP_OK;
}

bool unique_channels(const int *channels, uint8_t count)
{
    for (uint8_t i = 0; i < count; ++i) {
        for (uint8_t j = i + 1; j < count; ++j) {
            if (channels[i] == channels[j]) {
                return false;
            }
        }
    }
    return true;
}

bool gpio_reused_by_board(const BoardConfig &config, int gpio)
{
    if (config.i2c.present &&
        (gpio == config.i2c.sda_gpio || gpio == config.i2c.scl_gpio)) {
        return true;
    }
    if (config.relay.kind == RelayBackendKind::Gpio) {
        for (uint8_t i = 0; i < config.relay.count; ++i) {
            if (gpio == config.relay.channels[i]) {
                return true;
            }
        }
    }
    if (config.digital_input.kind == DigitalInputBackendKind::Gpio) {
        for (uint8_t i = 0; i < config.digital_input.count; ++i) {
            if (gpio == config.digital_input.channels[i]) {
                return true;
            }
        }
    }
    return false;
}

bool ethernet_uses_gpio(const EthernetHardwareConfig &ethernet, int gpio)
{
    return ethernet.kind == EthernetHardwareKind::W5500 &&
           (gpio == ethernet.mosi_gpio ||
            gpio == ethernet.miso_gpio ||
            gpio == ethernet.sclk_gpio ||
            gpio == ethernet.cs_gpio ||
            gpio == ethernet.interrupt_gpio ||
            gpio == ethernet.reset_gpio);
}

bool relay_uses_gpio(const RelayHardwareConfig &relay, int gpio)
{
    if (relay.kind != RelayBackendKind::Gpio) {
        return false;
    }
    for (uint8_t i = 0; i < relay.count; ++i) {
        if (gpio == relay.channels[i]) {
            return true;
        }
    }
    return false;
}

esp_err_t validate_config(const BoardConfig &config)
{
    ESP_RETURN_ON_FALSE(config.schema_version == kPower4BoardSchemaVersion,
                        ESP_ERR_INVALID_VERSION,
                        kTag,
                        "unsupported board schema %u",
                        config.schema_version);
    ESP_RETURN_ON_FALSE(config.relay.kind != RelayBackendKind::None &&
                            config.relay.count > 0 &&
                            config.relay.count <= kPower4MaxRelays,
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "invalid relay configuration");
    ESP_RETURN_ON_FALSE(config.relay.active_level <= 1,
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "invalid relay active level");
    ESP_RETURN_ON_FALSE(unique_channels(config.relay.channels, config.relay.count),
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "relay channels are not unique");

    if (config.relay.kind == RelayBackendKind::Gpio) {
        for (uint8_t i = 0; i < config.relay.count; ++i) {
            ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(config.relay.channels[i]),
                                ESP_ERR_INVALID_ARG,
                                kTag,
                                "relay GPIO %d is not output-capable",
                                config.relay.channels[i]);
        }
    } else {
        ESP_RETURN_ON_FALSE(config.relay.kind == RelayBackendKind::Tca9554 &&
                                config.i2c.present &&
                                config.relay.i2c_address > 0 &&
                                config.relay.i2c_address <= 0x7f,
                            ESP_ERR_INVALID_ARG,
                            kTag,
                            "invalid TCA9554 configuration");
        for (uint8_t i = 0; i < config.relay.count; ++i) {
            ESP_RETURN_ON_FALSE(config.relay.channels[i] >= 0 &&
                                    config.relay.channels[i] < 8,
                                ESP_ERR_INVALID_ARG,
                                kTag,
                                "invalid TCA9554 channel");
        }
    }

    if (config.i2c.present) {
        ESP_RETURN_ON_FALSE(config.i2c.port >= 0 &&
                                GPIO_IS_VALID_GPIO(config.i2c.sda_gpio) &&
                                GPIO_IS_VALID_GPIO(config.i2c.scl_gpio) &&
                                config.i2c.sda_gpio != config.i2c.scl_gpio &&
                                config.i2c.frequency_hz > 0 &&
                                config.i2c.frequency_hz <= 400000,
                            ESP_ERR_INVALID_ARG,
                            kTag,
                            "invalid I2C configuration");
    }

    if (config.digital_input.kind == DigitalInputBackendKind::Gpio) {
        ESP_RETURN_ON_FALSE(config.digital_input.count > 0 &&
                                config.digital_input.count <= kPower4MaxDigitalInputs &&
                                config.digital_input.active_level <= 1 &&
                                (config.digital_input.pull == GpioPullKind::None ||
                                 config.digital_input.pull == GpioPullKind::Up ||
                                 config.digital_input.pull == GpioPullKind::Down),
                            ESP_ERR_INVALID_ARG,
                            kTag,
                            "invalid digital input configuration");
        ESP_RETURN_ON_FALSE(unique_channels(config.digital_input.channels,
                                             config.digital_input.count),
                            ESP_ERR_INVALID_ARG,
                            kTag,
                            "digital input channels are not unique");
        for (uint8_t i = 0; i < config.digital_input.count; ++i) {
            const int gpio = config.digital_input.channels[i];
            ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(gpio),
                                ESP_ERR_INVALID_ARG,
                                kTag,
                                "digital input GPIO %d is invalid",
                                gpio);
            ESP_RETURN_ON_FALSE(!(config.i2c.present &&
                                  (gpio == config.i2c.sda_gpio ||
                                   gpio == config.i2c.scl_gpio)) &&
                                    !relay_uses_gpio(config.relay, gpio) &&
                                    !ethernet_uses_gpio(config.ethernet, gpio),
                                ESP_ERR_INVALID_ARG,
                                kTag,
                                "digital input GPIO %d conflicts with another board function",
                                gpio);
        }
    } else {
        ESP_RETURN_ON_FALSE(config.digital_input.kind == DigitalInputBackendKind::None &&
                                config.digital_input.count == 0,
                            ESP_ERR_INVALID_ARG,
                            kTag,
                            "invalid digital input backend");
    }

    if (config.ethernet.kind == EthernetHardwareKind::W5500) {
        const int pins[] = {
            config.ethernet.mosi_gpio,
            config.ethernet.miso_gpio,
            config.ethernet.sclk_gpio,
            config.ethernet.cs_gpio,
            config.ethernet.interrupt_gpio,
            config.ethernet.reset_gpio,
        };
        ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(config.ethernet.mosi_gpio) &&
                                GPIO_IS_VALID_GPIO(config.ethernet.miso_gpio) &&
                                GPIO_IS_VALID_OUTPUT_GPIO(config.ethernet.sclk_gpio) &&
                                GPIO_IS_VALID_OUTPUT_GPIO(config.ethernet.cs_gpio) &&
                                GPIO_IS_VALID_GPIO(config.ethernet.interrupt_gpio) &&
                                GPIO_IS_VALID_OUTPUT_GPIO(config.ethernet.reset_gpio),
                            ESP_ERR_INVALID_ARG,
                            kTag,
                            "invalid W5500 GPIO capability");
        for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
            ESP_RETURN_ON_FALSE(!gpio_reused_by_board(config, pins[i]),
                                ESP_ERR_INVALID_ARG,
                                kTag,
                                "W5500 GPIO %d conflicts with another board function",
                                pins[i]);
            for (size_t j = i + 1; j < sizeof(pins) / sizeof(pins[0]); ++j) {
                ESP_RETURN_ON_FALSE(pins[i] != pins[j],
                                    ESP_ERR_INVALID_ARG,
                                    kTag,
                                    "W5500 GPIOs are not unique");
            }
        }
        ESP_RETURN_ON_FALSE(config.ethernet.spi_host > 0 &&
                                config.ethernet.spi_host <= 2 &&
                                config.ethernet.phy_address <= 31 &&
                                config.ethernet.spi_frequency_hz > 0 &&
                                config.ethernet.spi_frequency_hz <= 80000000,
                            ESP_ERR_INVALID_ARG,
                            kTag,
                            "invalid W5500 configuration");
    }

    if (config.rtc.kind == RtcHardwareKind::Pcf85063a) {
        ESP_RETURN_ON_FALSE(config.i2c.present &&
                                config.rtc.i2c_address > 0 &&
                                config.rtc.i2c_address <= 0x7f &&
                                (config.relay.kind != RelayBackendKind::Tca9554 ||
                                 config.rtc.i2c_address != config.relay.i2c_address),
                            ESP_ERR_INVALID_ARG,
                            kTag,
                            "invalid RTC configuration");
    }

    return ESP_OK;
}

esp_err_t load_config(nvs_handle_t handle, BoardConfig *config)
{
    ESP_RETURN_ON_ERROR(nvs_get_u16(handle, "schema", &config->schema_version),
                        kTag,
                        "missing schema");
    ESP_RETURN_ON_ERROR(get_string(handle, "profile", config->profile, sizeof(config->profile)),
                        kTag,
                        "missing profile");
    ESP_RETURN_ON_ERROR(get_string(handle, "model", config->model, sizeof(config->model)),
                        kTag,
                        "missing model");

    char kind[16] = {};
    ESP_RETURN_ON_ERROR(get_string(handle, "relay_kind", kind, sizeof(kind)),
                        kTag,
                        "missing relay kind");
    if (strcmp(kind, "gpio") == 0) {
        config->relay.kind = RelayBackendKind::Gpio;
    } else if (strcmp(kind, "tca9554") == 0) {
        config->relay.kind = RelayBackendKind::Tca9554;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_RETURN_ON_ERROR(get_u8(handle, "relay_count", &config->relay.count),
                        kTag,
                        "missing relay count");
    ESP_RETURN_ON_ERROR(get_u8(handle, "relay_active", &config->relay.active_level),
                        kTag,
                        "missing relay active level");
    char map[64] = {};
    ESP_RETURN_ON_ERROR(get_string(handle, "relay_map", map, sizeof(map)),
                        kTag,
                        "missing relay map");
    ESP_RETURN_ON_ERROR(parse_channel_map(map, config->relay.count, config->relay.channels),
                        kTag,
                        "invalid relay map");
    if (config->relay.kind == RelayBackendKind::Tca9554) {
        ESP_RETURN_ON_ERROR(get_u8(handle, "tca_addr", &config->relay.i2c_address),
                            kTag,
                            "missing TCA9554 address");
    }

    uint8_t present = 0;
    ESP_RETURN_ON_ERROR(get_u8(handle, "i2c_present", &present), kTag, "missing I2C presence");
    config->i2c.present = present != 0;
    if (config->i2c.present) {
        ESP_RETURN_ON_ERROR(get_u32_as_int(handle, "i2c_port", &config->i2c.port),
                            kTag,
                            "missing I2C port");
        ESP_RETURN_ON_ERROR(get_u32_as_int(handle, "i2c_sda", &config->i2c.sda_gpio),
                            kTag,
                            "missing I2C SDA");
        ESP_RETURN_ON_ERROR(get_u32_as_int(handle, "i2c_scl", &config->i2c.scl_gpio),
                            kTag,
                            "missing I2C SCL");
        ESP_RETURN_ON_ERROR(nvs_get_u32(handle, "i2c_hz", &config->i2c.frequency_hz),
                            kTag,
                            "missing I2C frequency");
    }

    memset(kind, 0, sizeof(kind));
    ESP_RETURN_ON_ERROR(get_string(handle, "eth_kind", kind, sizeof(kind)),
                        kTag,
                        "missing Ethernet kind");
    if (strcmp(kind, "none") == 0) {
        config->ethernet.kind = EthernetHardwareKind::None;
    } else if (strcmp(kind, "w5500") == 0) {
        config->ethernet.kind = EthernetHardwareKind::W5500;
        ESP_RETURN_ON_ERROR(get_u32_as_int(handle, "eth_spi_host", &config->ethernet.spi_host),
                            kTag,
                            "missing Ethernet SPI host");
        ESP_RETURN_ON_ERROR(get_u32_as_int(handle, "eth_mosi", &config->ethernet.mosi_gpio),
                            kTag,
                            "missing Ethernet MOSI");
        ESP_RETURN_ON_ERROR(get_u32_as_int(handle, "eth_miso", &config->ethernet.miso_gpio),
                            kTag,
                            "missing Ethernet MISO");
        ESP_RETURN_ON_ERROR(get_u32_as_int(handle, "eth_sclk", &config->ethernet.sclk_gpio),
                            kTag,
                            "missing Ethernet SCLK");
        ESP_RETURN_ON_ERROR(get_u32_as_int(handle, "eth_cs", &config->ethernet.cs_gpio),
                            kTag,
                            "missing Ethernet CS");
        ESP_RETURN_ON_ERROR(get_u32_as_int(handle, "eth_int", &config->ethernet.interrupt_gpio),
                            kTag,
                            "missing Ethernet interrupt");
        ESP_RETURN_ON_ERROR(get_u32_as_int(handle, "eth_reset", &config->ethernet.reset_gpio),
                            kTag,
                            "missing Ethernet reset");
        ESP_RETURN_ON_ERROR(get_u8(handle, "eth_phy_addr", &config->ethernet.phy_address),
                            kTag,
                            "missing Ethernet PHY address");
        ESP_RETURN_ON_ERROR(nvs_get_u32(handle,
                                        "eth_spi_hz",
                                        &config->ethernet.spi_frequency_hz),
                            kTag,
                            "missing Ethernet SPI frequency");
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(kind, 0, sizeof(kind));
    esp_err_t input_kind_err = get_string(handle, "input_kind", kind, sizeof(kind));
    if (input_kind_err == ESP_ERR_NVS_NOT_FOUND || strcmp(kind, "none") == 0) {
        config->digital_input.kind = DigitalInputBackendKind::None;
    } else if (input_kind_err != ESP_OK) {
        return input_kind_err;
    } else if (strcmp(kind, "gpio") == 0) {
        config->digital_input.kind = DigitalInputBackendKind::Gpio;
        ESP_RETURN_ON_ERROR(get_u8(handle, "input_count", &config->digital_input.count),
                            kTag,
                            "missing digital input count");
        ESP_RETURN_ON_ERROR(get_u8(handle, "input_active", &config->digital_input.active_level),
                            kTag,
                            "missing digital input active level");

        char pull[8] = {};
        ESP_RETURN_ON_ERROR(get_string(handle, "input_pull", pull, sizeof(pull)),
                            kTag,
                            "missing digital input pull mode");
        if (strcmp(pull, "none") == 0) {
            config->digital_input.pull = GpioPullKind::None;
        } else if (strcmp(pull, "up") == 0) {
            config->digital_input.pull = GpioPullKind::Up;
        } else if (strcmp(pull, "down") == 0) {
            config->digital_input.pull = GpioPullKind::Down;
        } else {
            return ESP_ERR_NOT_SUPPORTED;
        }

        char input_map[64] = {};
        ESP_RETURN_ON_ERROR(get_string(handle, "input_map", input_map, sizeof(input_map)),
                            kTag,
                            "missing digital input map");
        ESP_RETURN_ON_ERROR(parse_channel_map(input_map,
                                               config->digital_input.count,
                                               config->digital_input.channels),
                            kTag,
                            "invalid digital input map");
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(kind, 0, sizeof(kind));
    ESP_RETURN_ON_ERROR(get_string(handle, "rtc_kind", kind, sizeof(kind)),
                        kTag,
                        "missing RTC kind");
    if (strcmp(kind, "none") == 0) {
        config->rtc.kind = RtcHardwareKind::None;
    } else if (strcmp(kind, "pcf85063a") == 0) {
        config->rtc.kind = RtcHardwareKind::Pcf85063a;
        ESP_RETURN_ON_ERROR(get_u8(handle, "rtc_addr", &config->rtc.i2c_address),
                            kTag,
                            "missing RTC address");
        uint8_t cap = 0;
        ESP_RETURN_ON_ERROR(get_u8(handle, "rtc_cap", &cap), kTag, "missing RTC capacitance");
        ESP_RETURN_ON_FALSE(cap <= 1, ESP_ERR_INVALID_ARG, kTag, "invalid RTC capacitance");
        config->rtc.capacitor_12_5_pf = cap != 0;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return validate_config(*config);
}

}  // namespace

esp_err_t board_config_init(void)
{
    if (g_result == ESP_OK) {
        return ESP_OK;
    }

    g_config = {};
    esp_err_t err = nvs_flash_init_partition(kPartition);
    if (err != ESP_OK) {
        set_error(err, "board_config partition is missing or unreadable");
        return err;
    }

    nvs_handle_t handle = 0;
    err = nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        set_error(err, "board_config namespace is missing");
        return err;
    }

    BoardConfig loaded = {};
    err = load_config(handle, &loaded);
    nvs_close(handle);
    if (err != ESP_OK) {
        set_error(err, "board_config values are invalid");
        return err;
    }

    g_config = loaded;
    g_result = ESP_OK;
    g_error[0] = '\0';
    ESP_LOGI(kTag,
             "loaded profile=%s model=\"%s\" relays=%u backend=%s inputs=%u",
             g_config.profile,
             g_config.model,
             g_config.relay.count,
             relay_backend_name(g_config.relay.kind),
             g_config.digital_input.count);
    return ESP_OK;
}

const BoardConfig *board_config_get(void)
{
    return g_result == ESP_OK ? &g_config : nullptr;
}

esp_err_t board_config_result(void)
{
    return g_result;
}

const char *board_config_error(void)
{
    return g_error;
}

const char *relay_backend_name(RelayBackendKind kind)
{
    switch (kind) {
    case RelayBackendKind::Gpio:
        return "gpio";
    case RelayBackendKind::Tca9554:
        return "tca9554";
    case RelayBackendKind::None:
        break;
    }
    return "none";
}

const char *ethernet_hardware_name(EthernetHardwareKind kind)
{
    return kind == EthernetHardwareKind::W5500 ? "w5500" : "none";
}

const char *rtc_hardware_name(RtcHardwareKind kind)
{
    return kind == RtcHardwareKind::Pcf85063a ? "pcf85063a" : "none";
}

const char *digital_input_backend_name(DigitalInputBackendKind kind)
{
    return kind == DigitalInputBackendKind::Gpio ? "gpio" : "none";
}

const char *gpio_pull_name(GpioPullKind kind)
{
    switch (kind) {
    case GpioPullKind::Up:
        return "up";
    case GpioPullKind::Down:
        return "down";
    case GpioPullKind::None:
        break;
    }
    return "none";
}
