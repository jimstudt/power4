#include "ethernet_manager.hpp"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/def.h"
#include "lwip/dns.h"
#include "nvs.h"

namespace {

constexpr const char *kTag = "ethernet";
constexpr const char *kNamespace = "ethernet";

EthernetHardwareConfig g_hardware = {};
EthernetSettings g_settings = {};
EthernetStatus g_status = {};
esp_eth_handle_t g_eth = nullptr;
esp_netif_t *g_netif = nullptr;
SemaphoreHandle_t g_mutex = nullptr;
StaticSemaphore_t g_mutex_storage = {};
bool g_driver_started = false;

void status_error(esp_err_t err)
{
    if (g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_status.last_error = err;
        xSemaphoreGive(g_mutex);
    }
}

void copy_ip_to_dns(const esp_ip4_addr_t &address, esp_netif_dns_info_t *dns)
{
    memset(dns, 0, sizeof(*dns));
    dns->ip.type = ESP_IPADDR_TYPE_V4;
    dns->ip.u_addr.ip4 = address;
}

esp_err_t clear_dns_server(void *context)
{
    const auto type = *static_cast<const esp_netif_dns_type_t *>(context);
    dns_setserver(static_cast<u8_t>(type), nullptr);
    return ESP_OK;
}

esp_err_t apply_optional_dns(esp_netif_dns_type_t type, const esp_ip4_addr_t &address)
{
    if (address.addr == 0) {
        // esp_netif_set_dns_info() rejects an all-zero address. Clear the
        // corresponding lwIP slot on the TCP/IP thread instead.
        return esp_netif_tcpip_exec(clear_dns_server, &type);
    }

    esp_netif_dns_info_t dns = {};
    copy_ip_to_dns(address, &dns);
    return esp_netif_set_dns_info(g_netif, type, &dns);
}

bool netmask_valid(const esp_ip4_addr_t &address)
{
    const uint32_t mask = lwip_ntohl(address.addr);
    if (mask == 0) {
        return false;
    }
    const uint32_t inverse = ~mask;
    return (inverse & (inverse + 1U)) == 0;
}

bool unicast_or_zero(const esp_ip4_addr_t &address)
{
    const uint32_t host = lwip_ntohl(address.addr);
    if (host == 0) {
        return true;
    }
    const uint8_t first = static_cast<uint8_t>(host >> 24);
    return first != 0 && first < 224 && host != UINT32_MAX;
}

esp_err_t validate_settings(const EthernetSettings &settings)
{
    ESP_RETURN_ON_FALSE(static_cast<uint8_t>(settings.phy_mode) <=
                            static_cast<uint8_t>(EthernetPhyMode::Speed100Full),
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "invalid PHY mode");
    if (settings.address_mode == EthernetAddressMode::Dhcp) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(settings.address_mode == EthernetAddressMode::Static &&
                            settings.ip.addr != 0 &&
                            unicast_or_zero(settings.ip) &&
                            netmask_valid(settings.netmask) &&
                            unicast_or_zero(settings.gateway) &&
                            unicast_or_zero(settings.dns1) &&
                            unicast_or_zero(settings.dns2),
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "invalid static IPv4 configuration");
    if (settings.gateway.addr != 0) {
        ESP_RETURN_ON_FALSE((settings.gateway.addr & settings.netmask.addr) ==
                                (settings.ip.addr & settings.netmask.addr),
                            ESP_ERR_INVALID_ARG,
                            kTag,
                            "gateway is outside the local subnet");
    }
    return ESP_OK;
}

esp_err_t load_settings(EthernetSettings *settings)
{
    *settings = {};
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t address_mode = 0;
    uint8_t phy_mode = 0;
    err = nvs_get_u8(handle, "addr_mode", &address_mode);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err == ESP_OK) {
        settings->address_mode = static_cast<EthernetAddressMode>(address_mode);
        err = nvs_get_u8(handle, "phy_mode", &phy_mode);
        if (err == ESP_OK) {
            settings->phy_mode = static_cast<EthernetPhyMode>(phy_mode);
        }
        if (err == ESP_OK && settings->address_mode == EthernetAddressMode::Static) {
            err = nvs_get_u32(handle, "ip", &settings->ip.addr);
            if (err == ESP_OK) {
                err = nvs_get_u32(handle, "netmask", &settings->netmask.addr);
            }
            if (err == ESP_OK) {
                err = nvs_get_u32(handle, "gateway", &settings->gateway.addr);
            }
            if (err == ESP_OK) {
                err = nvs_get_u32(handle, "dns1", &settings->dns1.addr);
            }
            if (err == ESP_OK) {
                err = nvs_get_u32(handle, "dns2", &settings->dns2.addr);
            }
        }
    }
    nvs_close(handle);
    return err == ESP_OK ? validate_settings(*settings) : err;
}

esp_err_t save_settings(const EthernetSettings &settings)
{
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(kNamespace, NVS_READWRITE, &handle),
                        kTag,
                        "failed to open Ethernet settings");
    esp_err_t err = nvs_set_u8(handle,
                               "addr_mode",
                               static_cast<uint8_t>(settings.address_mode));
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "phy_mode", static_cast<uint8_t>(settings.phy_mode));
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "ip", settings.ip.addr);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "netmask", settings.netmask.addr);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "gateway", settings.gateway.addr);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "dns1", settings.dns1.addr);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "dns2", settings.dns2.addr);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t apply_address_settings(const EthernetSettings &settings)
{
    if (settings.address_mode == EthernetAddressMode::Dhcp) {
        esp_err_t err = esp_netif_dhcpc_start(g_netif);
        if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            err = ESP_OK;
        }
        return err;
    }

    esp_err_t err = esp_netif_dhcpc_stop(g_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }

    esp_netif_ip_info_t info = {};
    info.ip = settings.ip;
    info.netmask = settings.netmask;
    info.gw = settings.gateway;
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(g_netif, &info),
                        kTag,
                        "failed to set static IPv4 configuration");

    ESP_RETURN_ON_ERROR(apply_optional_dns(ESP_NETIF_DNS_MAIN, settings.dns1),
                        kTag,
                        "failed to set primary DNS");
    return apply_optional_dns(ESP_NETIF_DNS_BACKUP, settings.dns2);
}

esp_err_t apply_phy_mode(EthernetPhyMode mode)
{
    bool autonegotiation = mode == EthernetPhyMode::Auto;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(g_eth, ETH_CMD_S_AUTONEGO, &autonegotiation),
                        kTag,
                        "failed to configure PHY auto-negotiation");
    if (autonegotiation) {
        return ESP_OK;
    }

    eth_speed_t speed =
        mode == EthernetPhyMode::Speed10Half || mode == EthernetPhyMode::Speed10Full
            ? ETH_SPEED_10M
            : ETH_SPEED_100M;
    eth_duplex_t duplex =
        mode == EthernetPhyMode::Speed10Full || mode == EthernetPhyMode::Speed100Full
            ? ETH_DUPLEX_FULL
            : ETH_DUPLEX_HALF;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(g_eth, ETH_CMD_S_SPEED, &speed),
                        kTag,
                        "failed to set PHY speed");
    return esp_eth_ioctl(g_eth, ETH_CMD_S_DUPLEX_MODE, &duplex);
}

esp_err_t stop_driver(void)
{
    if (!g_driver_started) {
        return ESP_OK;
    }
    esp_err_t err = esp_eth_stop(g_eth);
    if (err == ESP_OK) {
        g_driver_started = false;
    }
    return err;
}

esp_err_t start_driver(void)
{
    if (g_driver_started) {
        return ESP_OK;
    }
    esp_err_t err = esp_eth_start(g_eth);
    if (err == ESP_OK) {
        g_driver_started = true;
    }
    return err;
}

esp_err_t apply_complete_settings(const EthernetSettings &settings)
{
    ESP_RETURN_ON_ERROR(apply_address_settings(settings),
                        kTag,
                        "failed to apply address settings");
    ESP_RETURN_ON_ERROR(apply_phy_mode(settings.phy_mode),
                        kTag,
                        "failed to apply PHY settings");
    return ESP_OK;
}

esp_err_t reconfigure(const EthernetSettings &requested)
{
    ESP_RETURN_ON_ERROR(validate_settings(requested), kTag, "invalid Ethernet settings");
    ESP_RETURN_ON_FALSE(g_status.initialized && g_eth != nullptr && g_netif != nullptr,
                        ESP_ERR_INVALID_STATE,
                        kTag,
                        "Ethernet is not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(g_mutex, pdMS_TO_TICKS(2000)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        kTag,
                        "Ethernet configuration is busy");

    const EthernetSettings previous = g_settings;
    esp_err_t err = stop_driver();
    if (err == ESP_OK) {
        err = apply_complete_settings(requested);
    }
    if (err == ESP_OK) {
        err = start_driver();
    }
    if (err == ESP_OK) {
        err = save_settings(requested);
    }

    if (err == ESP_OK) {
        g_settings = requested;
        g_status.settings = requested;
        g_status.last_error = ESP_OK;
    } else {
        ESP_LOGE(kTag, "configuration failed, restoring previous settings: %s", esp_err_to_name(err));
        stop_driver();
        apply_complete_settings(previous);
        start_driver();
        g_status.last_error = err;
    }

    xSemaphoreGive(g_mutex);
    return err;
}

void ethernet_event_handler(void *,
                            esp_event_base_t,
                            int32_t event_id,
                            void *)
{
    if (g_mutex == nullptr || xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    if (event_id == ETHERNET_EVENT_CONNECTED) {
        g_status.link_up = true;
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;
        if (esp_eth_ioctl(g_eth, ETH_CMD_G_SPEED, &speed) == ESP_OK) {
            g_status.speed_mbps = speed == ETH_SPEED_100M ? 100 : 10;
        }
        if (esp_eth_ioctl(g_eth, ETH_CMD_G_DUPLEX_MODE, &duplex) == ESP_OK) {
            g_status.full_duplex = duplex == ETH_DUPLEX_FULL;
        }
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED ||
               event_id == ETHERNET_EVENT_STOP) {
        g_status.link_up = false;
        g_status.has_ip = false;
        g_status.speed_mbps = 0;
    }
    xSemaphoreGive(g_mutex);
}

void ip_event_handler(void *,
                      esp_event_base_t,
                      int32_t event_id,
                      void *event_data)
{
    if (g_mutex == nullptr || xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    if (event_id == IP_EVENT_ETH_GOT_IP && event_data != nullptr) {
        const ip_event_got_ip_t *event = static_cast<const ip_event_got_ip_t *>(event_data);
        g_status.current_ip = event->ip_info;
        g_status.has_ip = true;
    } else if (event_id == IP_EVENT_ETH_LOST_IP) {
        g_status.current_ip = {};
        g_status.has_ip = false;
    }
    xSemaphoreGive(g_mutex);
}

}  // namespace

const char *ethernet_address_mode_name(EthernetAddressMode mode)
{
    return mode == EthernetAddressMode::Static ? "static" : "dhcp";
}

const char *ethernet_phy_mode_name(EthernetPhyMode mode)
{
    switch (mode) {
    case EthernetPhyMode::Auto:
        return "auto";
    case EthernetPhyMode::Speed10Half:
        return "10-half";
    case EthernetPhyMode::Speed10Full:
        return "10-full";
    case EthernetPhyMode::Speed100Half:
        return "100-half";
    case EthernetPhyMode::Speed100Full:
        return "100-full";
    }
    return "unknown";
}

bool ethernet_phy_mode_parse(const char *text, EthernetPhyMode *mode)
{
    if (text == nullptr || mode == nullptr) {
        return false;
    }
    for (uint8_t value = 0;
         value <= static_cast<uint8_t>(EthernetPhyMode::Speed100Full);
         ++value) {
        const auto candidate = static_cast<EthernetPhyMode>(value);
        if (strcmp(text, ethernet_phy_mode_name(candidate)) == 0) {
            *mode = candidate;
            return true;
        }
    }
    return false;
}

esp_err_t ethernet_manager_start(const BoardConfig &board)
{
    g_hardware = board.ethernet;
    g_status = {};
    g_status.present = g_hardware.kind == EthernetHardwareKind::W5500;
    if (!g_status.present) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    g_mutex = xSemaphoreCreateMutexStatic(&g_mutex_storage);
    ESP_RETURN_ON_FALSE(g_mutex != nullptr, ESP_ERR_NO_MEM, kTag, "failed to create mutex");

    esp_err_t settings_err = load_settings(&g_settings);
    if (settings_err != ESP_OK) {
        ESP_LOGW(kTag,
                 "stored settings are invalid; using DHCP and auto: %s",
                 esp_err_to_name(settings_err));
        g_settings = {};
    }
    g_status.settings = g_settings;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        status_error(err);
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        status_error(err);
        return err;
    }

    // The WIZnet MAC component registers a per-pin GPIO handler but assumes
    // the shared GPIO ISR service has already been installed. Without this,
    // gpio_isr_handler_add() fails inside the component and its RX task only
    // notices packets on the 1000 ms fallback wake-up.
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        status_error(err);
        ESP_LOGE(kTag, "failed to install GPIO ISR service: %s", esp_err_to_name(err));
        return err;
    }

    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = g_hardware.mosi_gpio;
    bus_config.miso_io_num = g_hardware.miso_gpio;
    bus_config.sclk_io_num = g_hardware.sclk_gpio;
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;
    err = spi_bus_initialize(static_cast<spi_host_device_t>(g_hardware.spi_host),
                             &bus_config,
                             SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        status_error(err);
        return err;
    }

    spi_device_interface_config_t spi_device = {};
    spi_device.mode = 0;
    spi_device.clock_speed_hz = g_hardware.spi_frequency_hz;
    spi_device.queue_size = 20;
    spi_device.spics_io_num = g_hardware.cs_gpio;

    eth_w5500_config_t w5500 =
        ETH_W5500_DEFAULT_CONFIG(static_cast<spi_host_device_t>(g_hardware.spi_host),
                                 &spi_device);
    w5500.base.int_gpio_num = g_hardware.interrupt_gpio;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_config);
    ESP_RETURN_ON_FALSE(mac != nullptr, ESP_ERR_NO_MEM, kTag, "failed to create W5500 MAC");

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = g_hardware.phy_address;
    phy_config.reset_gpio_num = g_hardware.reset_gpio;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    ESP_RETURN_ON_FALSE(phy != nullptr, ESP_ERR_NO_MEM, kTag, "failed to create W5500 PHY");

    esp_eth_config_t driver_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&driver_config, &g_eth),
                        kTag,
                        "failed to install Ethernet driver");

    uint8_t mac_address[6] = {};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac_address, ESP_MAC_ETH),
                        kTag,
                        "failed to derive Ethernet MAC");
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(g_eth, ETH_CMD_S_MAC_ADDR, mac_address),
                        kTag,
                        "failed to set Ethernet MAC");
    memcpy(g_status.mac, mac_address, sizeof(mac_address));

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    g_netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(g_netif != nullptr, ESP_ERR_NO_MEM, kTag, "failed to create netif");
    ESP_RETURN_ON_ERROR(esp_netif_attach(g_netif, esp_eth_new_netif_glue(g_eth)),
                        kTag,
                        "failed to attach Ethernet netif");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT,
                                                    ESP_EVENT_ANY_ID,
                                                    &ethernet_event_handler,
                                                    nullptr),
                        kTag,
                        "failed to register Ethernet events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                    IP_EVENT_ETH_GOT_IP,
                                                    &ip_event_handler,
                                                    nullptr),
                        kTag,
                        "failed to register Ethernet IP event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                    IP_EVENT_ETH_LOST_IP,
                                                    &ip_event_handler,
                                                    nullptr),
                        kTag,
                        "failed to register Ethernet lost-IP event");

    ESP_RETURN_ON_ERROR(apply_complete_settings(g_settings),
                        kTag,
                        "failed to apply stored Ethernet settings");

    // Publish initialization before starting the driver so event callbacks do
    // not race with these status fields.
    g_status.initialized = true;
    g_status.last_error = ESP_OK;
    err = start_driver();
    if (err != ESP_OK) {
        g_status.initialized = false;
        g_status.last_error = err;
        ESP_LOGE(kTag, "failed to start Ethernet: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(kTag,
             "W5500 started address=%s phy=%s interrupt_gpio=%d level=%d",
             ethernet_address_mode_name(g_settings.address_mode),
             ethernet_phy_mode_name(g_settings.phy_mode),
             g_hardware.interrupt_gpio,
             gpio_get_level(static_cast<gpio_num_t>(g_hardware.interrupt_gpio)));
    return ESP_OK;
}

esp_err_t ethernet_manager_get_status(EthernetStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_mutex == nullptr) {
        *status = g_status;
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        kTag,
                        "Ethernet status is busy");
    if (g_netif != nullptr) {
        esp_netif_get_ip_info(g_netif, &g_status.current_ip);
        esp_netif_get_dns_info(g_netif, ESP_NETIF_DNS_MAIN, &g_status.current_dns1);
        esp_netif_get_dns_info(g_netif, ESP_NETIF_DNS_BACKUP, &g_status.current_dns2);
    }
    *status = g_status;
    xSemaphoreGive(g_mutex);
    return ESP_OK;
}

esp_err_t ethernet_manager_set_dhcp(void)
{
    EthernetSettings requested = g_settings;
    requested.address_mode = EthernetAddressMode::Dhcp;
    requested.ip = {};
    requested.netmask = {};
    requested.gateway = {};
    requested.dns1 = {};
    requested.dns2 = {};
    return reconfigure(requested);
}

esp_err_t ethernet_manager_set_static(const char *ip,
                                      const char *netmask,
                                      const char *gateway,
                                      const char *dns1,
                                      const char *dns2)
{
    EthernetSettings requested = g_settings;
    requested.address_mode = EthernetAddressMode::Static;
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(ip, &requested.ip), kTag, "invalid IP address");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(netmask, &requested.netmask),
                        kTag,
                        "invalid netmask");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(gateway, &requested.gateway),
                        kTag,
                        "invalid gateway");
    requested.dns1 = {};
    requested.dns2 = {};
    if (dns1 != nullptr) {
        ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(dns1, &requested.dns1),
                            kTag,
                            "invalid primary DNS");
    }
    if (dns2 != nullptr) {
        ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(dns2, &requested.dns2),
                            kTag,
                            "invalid secondary DNS");
    }
    return reconfigure(requested);
}

esp_err_t ethernet_manager_set_phy(EthernetPhyMode mode)
{
    EthernetSettings requested = g_settings;
    requested.phy_mode = mode;
    return reconfigure(requested);
}
