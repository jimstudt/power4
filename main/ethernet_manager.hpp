#pragma once

#include <stdint.h>

#include "board_config.hpp"
#include "esp_err.h"
#include "esp_netif.h"

enum class EthernetAddressMode : uint8_t {
    Dhcp = 0,
    Static = 1,
};

enum class EthernetPhyMode : uint8_t {
    Auto = 0,
    Speed10Half,
    Speed10Full,
    Speed100Half,
    Speed100Full,
};

struct EthernetSettings {
    EthernetAddressMode address_mode = EthernetAddressMode::Dhcp;
    EthernetPhyMode phy_mode = EthernetPhyMode::Auto;
    esp_ip4_addr_t ip = {};
    esp_ip4_addr_t netmask = {};
    esp_ip4_addr_t gateway = {};
    esp_ip4_addr_t dns1 = {};
    esp_ip4_addr_t dns2 = {};
};

struct EthernetStatus {
    bool present = false;
    bool initialized = false;
    bool link_up = false;
    bool has_ip = false;
    bool full_duplex = false;
    uint16_t speed_mbps = 0;
    esp_err_t last_error = ESP_ERR_NOT_SUPPORTED;
    EthernetSettings settings = {};
    esp_netif_ip_info_t current_ip = {};
    esp_netif_dns_info_t current_dns1 = {};
    esp_netif_dns_info_t current_dns2 = {};
    uint8_t mac[6] = {};
};

esp_err_t ethernet_manager_start(const BoardConfig &board);
esp_err_t ethernet_manager_get_status(EthernetStatus *status);
esp_err_t ethernet_manager_set_dhcp(void);
esp_err_t ethernet_manager_set_static(const char *ip,
                                      const char *netmask,
                                      const char *gateway,
                                      const char *dns1,
                                      const char *dns2);
esp_err_t ethernet_manager_set_phy(EthernetPhyMode mode);

const char *ethernet_address_mode_name(EthernetAddressMode mode);
const char *ethernet_phy_mode_name(EthernetPhyMode mode);
bool ethernet_phy_mode_parse(const char *text, EthernetPhyMode *mode);
