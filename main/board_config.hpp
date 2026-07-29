#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

constexpr uint8_t kPower4MaxRelays = 8;
constexpr uint8_t kPower4MaxDigitalInputs = 8;
constexpr uint16_t kPower4BoardSchemaVersion = 1;

enum class RelayBackendKind : uint8_t {
    None,
    Gpio,
    Tca9554,
};

enum class EthernetHardwareKind : uint8_t {
    None,
    W5500,
};

enum class RtcHardwareKind : uint8_t {
    None,
    Pcf85063a,
};

enum class DigitalInputBackendKind : uint8_t {
    None,
    Gpio,
};

enum class GpioPullKind : uint8_t {
    None,
    Up,
    Down,
};

struct I2cHardwareConfig {
    bool present = false;
    int port = 0;
    int sda_gpio = -1;
    int scl_gpio = -1;
    uint32_t frequency_hz = 100000;
};

struct RelayHardwareConfig {
    RelayBackendKind kind = RelayBackendKind::None;
    uint8_t count = 0;
    uint8_t active_level = 1;
    int channels[kPower4MaxRelays] = {};
    uint8_t i2c_address = 0;
};

struct EthernetHardwareConfig {
    EthernetHardwareKind kind = EthernetHardwareKind::None;
    int spi_host = 0;
    int mosi_gpio = -1;
    int miso_gpio = -1;
    int sclk_gpio = -1;
    int cs_gpio = -1;
    int interrupt_gpio = -1;
    int reset_gpio = -1;
    uint8_t phy_address = 0;
    uint32_t spi_frequency_hz = 0;
};

struct RtcHardwareConfig {
    RtcHardwareKind kind = RtcHardwareKind::None;
    uint8_t i2c_address = 0;
    bool capacitor_12_5_pf = false;
};

struct DigitalInputHardwareConfig {
    DigitalInputBackendKind kind = DigitalInputBackendKind::None;
    uint8_t count = 0;
    uint8_t active_level = 1;
    GpioPullKind pull = GpioPullKind::None;
    int channels[kPower4MaxDigitalInputs] = {};
};

struct BoardConfig {
    uint16_t schema_version = 0;
    char profile[24] = {};
    char model[64] = {};
    RelayHardwareConfig relay = {};
    I2cHardwareConfig i2c = {};
    EthernetHardwareConfig ethernet = {};
    RtcHardwareConfig rtc = {};
    DigitalInputHardwareConfig digital_input = {};
};

esp_err_t board_config_init(void);
const BoardConfig *board_config_get(void);
esp_err_t board_config_result(void);
const char *board_config_error(void);

const char *relay_backend_name(RelayBackendKind kind);
const char *ethernet_hardware_name(EthernetHardwareKind kind);
const char *rtc_hardware_name(RtcHardwareKind kind);
const char *digital_input_backend_name(DigitalInputBackendKind kind);
const char *gpio_pull_name(GpioPullKind kind);
