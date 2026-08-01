#include "console.hpp"

#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "battery_bank.hpp"
#include "battery_scanner.hpp"
#include "battery_store.hpp"
#include "board_config.hpp"
#include "checksum.hpp"
#include "config_flags.hpp"
#include "ethernet_manager.hpp"
#include "espnow_manager.hpp"
#include "espnow_protocol.hpp"
#include "input_manager.hpp"
#include "json_output.hpp"
#include "log_buffer.hpp"
#include "policy_storage.hpp"
#include "policy_task.hpp"
#include "relay_manager.hpp"
#include "rtc_manager.hpp"
#include "state_report.hpp"
#include "time_manager.hpp"
#include "network_console.hpp"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_log_write.h"
#include "esp_system.h"
#include "esp_timer.h"
#if CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif
#if CONFIG_ESP_CONSOLE_USB_CDC
#include "esp_vfs_cdcacm.h"
#endif
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

extern "C" {
#include "host/ble_gap.h"
#include "host/ble_hs.h"

void ble_gap_conn_foreach_handle(ble_gap_conn_foreach_handle_fn *cb, void *arg);
}

namespace {

constexpr const char *kTag = "power4_console";
constexpr const char *kPrompt = "power4> ";
constexpr size_t kParameterStateJsonBaseBytes = 128;
constexpr size_t kParameterStateJsonBytesPerParameter = 256;
constexpr size_t kLogsJsonBaseBytes = 96;
constexpr size_t kPolicyUploadMaxDecodedBytes = kPolicyProgramMaxBytes;
constexpr size_t kPolicyUploadMaxEncodedBytes = ((kPolicyUploadMaxDecodedBytes + 2) / 3) * 4;
// The USB serial JTAG driver's ISR silently drops received bytes when its RX
// ring buffer is full; there is no flow control back to the host. Tools like
// power4ctl send a whole policy upload in one burst, so the ring must hold
// the largest burst: the encoded policy plus line terminators and command.
// usb_serial_jtag_driver_install() creates this ring on the heap.
constexpr size_t kConsoleRxBufferBytes = kPolicyUploadMaxEncodedBytes + 1024;
constexpr size_t kPolicyUploadLineBytes = 160;
constexpr size_t kConsoleLineBytes = 256;
constexpr uint32_t kConsoleTaskStackBytes = 8192;
constexpr UBaseType_t kConsoleTaskPriority = 4;
constexpr TickType_t kCommandMutexTimeoutTicks = pdMS_TO_TICKS(2000);
constexpr char kToolCommandPrefix[] = "p4exec ";

portMUX_TYPE g_console_line_mux = portMUX_INITIALIZER_UNLOCKED;
char g_console_line[kConsoleLineBytes] = {};
size_t g_console_line_length = 0;
bool g_console_prompt_active = false;
vprintf_like_t g_previous_log_vprintf = nullptr;
StaticSemaphore_t g_command_mutex_storage = {};
SemaphoreHandle_t g_command_mutex = nullptr;
Power4CommandSource g_command_source = Power4CommandSource::Serial;
TaskHandle_t g_capture_task = nullptr;
esp_timer_handle_t g_reboot_timer = nullptr;

struct DotStuffStream {
    FILE *destination;
    bool line_start;
};

int bank_show_command(void);
int ble_show_command(void);
int print_policy_slot(PolicySlot slot);
int system_command(int argc, char **argv);
int reboot_command(int argc, char **argv);

enum class EscapeState : uint8_t {
    None,
    Esc,
    Csi,
};

bool parse_u32(const char *text, uint32_t *value)
{
    if (text == nullptr || value == nullptr || text[0] == '\0') {
        return false;
    }

    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }

    *value = static_cast<uint32_t>(parsed);
    return true;
}

void draw_prompt_line(const char *line, size_t length)
{
    printf("%s", kPrompt);
    if (length > 0) {
        fwrite(line, 1, length, stdout);
    }
    fflush(stdout);
}

void redraw_line(const char *line, size_t length)
{
    printf("\r\n");
    draw_prompt_line(line, length);
}

void set_console_line_snapshot(const char *line, size_t length, bool prompt_active)
{
    if (line == nullptr) {
        length = 0;
    }
    if (length >= sizeof(g_console_line)) {
        length = sizeof(g_console_line) - 1;
    }

    portENTER_CRITICAL(&g_console_line_mux);
    if (length > 0) {
        memcpy(g_console_line, line, length);
    }
    g_console_line[length] = '\0';
    g_console_line_length = length;
    g_console_prompt_active = prompt_active;
    portEXIT_CRITICAL(&g_console_line_mux);
}

bool copy_console_line_snapshot(char *line, size_t capacity, size_t *length, bool *prompt_active)
{
    if (line == nullptr || capacity == 0 || length == nullptr || prompt_active == nullptr) {
        return false;
    }

    portENTER_CRITICAL(&g_console_line_mux);
    const size_t copy_length =
        g_console_line_length < capacity ? g_console_line_length : capacity - 1;
    if (copy_length > 0) {
        memcpy(line, g_console_line, copy_length);
    }
    line[copy_length] = '\0';
    *length = copy_length;
    *prompt_active = g_console_prompt_active;
    portEXIT_CRITICAL(&g_console_line_mux);

    return true;
}

void update_console_line_snapshot(const char *line, size_t length, bool prompt_active)
{
    set_console_line_snapshot(line, length, prompt_active);
}

void erase_one_display_char(void)
{
    printf("\b \b");
    fflush(stdout);
}

void clear_input_line(char *line, size_t *length)
{
    if (line == nullptr || length == nullptr) {
        return;
    }

    while (*length > 0) {
        --(*length);
        line[*length] = '\0';
        erase_one_display_char();
    }
}

bool consume_escape_sequence(char ch, EscapeState *state)
{
    if (state == nullptr) {
        return false;
    }

    switch (*state) {
    case EscapeState::None:
        if (ch == '\x1b') {
            *state = EscapeState::Esc;
            return true;
        }
        return false;

    case EscapeState::Esc:
        *state = ch == '[' ? EscapeState::Csi : EscapeState::None;
        return true;

    case EscapeState::Csi:
        if ((ch >= '@' && ch <= '~')) {
            *state = EscapeState::None;
        }
        return true;
    }

    *state = EscapeState::None;
    return false;
}

int console_log_vprintf(const char *format, va_list args)
{
    if (g_capture_task == xTaskGetCurrentTaskHandle()) {
        return g_previous_log_vprintf != nullptr ? g_previous_log_vprintf(format, args)
                                                 : vprintf(format, args);
    }

    char line[kConsoleLineBytes] = {};
    size_t length = 0;
    bool prompt_active = false;
    copy_console_line_snapshot(line, sizeof(line), &length, &prompt_active);

    if (prompt_active) {
        if (length > 0) {
            printf(" ...\r\n");
        } else {
            printf("\r\n");
        }
        fflush(stdout);
    }

    const int result = g_previous_log_vprintf != nullptr ? g_previous_log_vprintf(format, args)
                                                         : vprintf(format, args);

    if (prompt_active) {
        draw_prompt_line(line, length);
    }

    return result;
}

void reboot_timer_callback(void *)
{
    esp_restart();
}

int dot_stuff_write(void *cookie, const char *buffer, int length)
{
    auto *stream = static_cast<DotStuffStream *>(cookie);
    size_t run_start = 0;
    for (int i = 0; i < length; ++i) {
        if (stream->line_start && buffer[i] == '.') {
            const size_t run_length = static_cast<size_t>(i) - run_start;
            if (run_length > 0 &&
                fwrite(buffer + run_start, 1, run_length, stream->destination) !=
                    run_length) {
                return -1;
            }
            if (fwrite(".", 1, 1, stream->destination) != 1) {
                return -1;
            }
            run_start = static_cast<size_t>(i);
        }
        stream->line_start = buffer[i] == '\n';
    }

    const size_t remaining = static_cast<size_t>(length) - run_start;
    if (remaining > 0 &&
        fwrite(buffer + run_start, 1, remaining, stream->destination) != remaining) {
        return -1;
    }
    return length;
}

bool finish_dot_stuffed_response(DotStuffStream *stream)
{
    if (!stream->line_start && fwrite("\n", 1, 1, stream->destination) != 1) {
        return false;
    }
    return fwrite(".\n", 1, 2, stream->destination) == 2 &&
           fflush(stream->destination) == 0;
}

void write_console_busy_frame(FILE *destination)
{
    if (destination == nullptr) {
        return;
    }
    fputs("console busy; try again\n.\n", destination);
    fflush(destination);
}

bool parse_relay(const char *text, uint8_t *relay)
{
    uint32_t parsed = 0;
    if (!parse_u32(text, &parsed) || parsed == 0 || parsed > relay_manager_count()) {
        return false;
    }

    *relay = static_cast<uint8_t>(parsed);
    return true;
}

void print_relay_status(const RelayStatus &status)
{
    printf("relay %u: backend=%s ",
           status.relay,
           relay_backend_name(status.backend));
    if (status.backend == RelayBackendKind::Gpio) {
        printf("gpio=%d ", status.gpio_pin);
    } else if (status.backend == RelayBackendKind::Tca9554) {
        printf("exio=%d ", status.hardware_channel + 1);
    }
    printf("active_level=%u output=%s timer=%s",
           status.active_level,
           status.output_on ? "on" : "off",
           status.timer_active ? "on" : "off");
    if (status.timer_active) {
        printf(" remaining=%" PRIu32 "s", status.timer_remaining_s);
    }
    printf(" force=%s\n", relay_force_name(status.force));
}

int query_and_print_relay(uint8_t relay)
{
    RelayStatus status = {};
    const esp_err_t err = relay_manager_query(relay, &status);
    if (err != ESP_OK) {
        printf("relay query failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    print_relay_status(status);
    return 0;
}

int print_all_relays(void)
{
    if (relay_manager_count() == 0) {
        printf("relays: unavailable\n");
        return 1;
    }

    int result = 0;
    for (uint8_t relay = 1; relay <= relay_manager_count(); ++relay) {
        result |= query_and_print_relay(relay);
    }
    return result;
}

int print_buffered_logs(void)
{
    char *text = static_cast<char *>(malloc(kLogBufferBytes));
    if (text == nullptr) {
        printf("show logs failed: out of memory\n");
        return 1;
    }

    const size_t length = log_buffer_snapshot(text, kLogBufferBytes);
    if (length == 0) {
        printf("log buffer is empty\n");
    } else {
        fwrite(text, 1, length, stdout);
        if (text[length - 1] != '\n') {
            printf("\n");
        }
    }

    free(text);
    return 0;
}

void config_flag_sort_order(const ConfigFlagList &flags, size_t *order)
{
    for (size_t i = 0; i < flags.count; ++i) {
        size_t j = i;
        while (j > 0 && strcmp(flags.names[i], flags.names[order[j - 1]]) < 0) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = i;
    }
}

int print_config_flags(void)
{
    ConfigFlagList *flags = static_cast<ConfigFlagList *>(malloc(sizeof(ConfigFlagList)));
    if (flags == nullptr) {
        printf("parameters: error:out of memory\n");
        return 1;
    }

    const esp_err_t err = config_flags_list(flags);
    if (err != ESP_OK) {
        printf("parameters: error:%s\n", esp_err_to_name(err));
        free(flags);
        return 1;
    }

    // NVS iteration order is arbitrary; present the names alphabetically.
    size_t order[kConfigFlagListMax];
    config_flag_sort_order(*flags, order);

    if (flags->count == 0) {
        printf("parameters: none\n");
    }
    for (size_t i = 0; i < flags->count; ++i) {
        const size_t k = order[i];
        printf("%s", flags->names[k]);
        if (flags->values[k][0] != '\0') {
            printf("=%s", flags->values[k]);
        }
        if (flags->lifetime_s[k] > 0) {
            printf(" (%u/%us)",
                   static_cast<unsigned>(flags->remaining_s[k]),
                   static_cast<unsigned>(flags->lifetime_s[k]));
        }
        printf("\n");
    }
    if (flags->truncated) {
        printf("...\n");
    }

    const bool truncated = flags->truncated;
    free(flags);
    return truncated ? 1 : 0;
}

void print_show_usage(void)
{
    printf("usage:\n");
    printf("  show batteries\n");
    printf("  show banks\n");
    printf("  show ble\n");
    printf("  show board\n");
    printf("  show debug\n");
    printf("  show ethernet\n");
    printf("  show espnow\n");
    printf("  show inputs\n");
    printf("  show logs\n");
    printf("  show password\n");
    printf("  show policy\n");
    printf("  show policy parameters\n");
    printf("  show policy staged\n");
    printf("  show relays\n");
    printf("  show system\n");
    printf("  show time\n");
    printf("  show timezone\n");
    printf("  show timezones\n");
}

int show_batteries_command(void)
{
    BatteryRecord *records =
        static_cast<BatteryRecord *>(calloc(CONFIG_POWER4_MAX_BATTERIES, sizeof(BatteryRecord)));
    if (records == nullptr) {
        printf("show batteries failed: out of memory\n");
        return 1;
    }

    size_t count = 0;
    const esp_err_t err = battery_store_snapshot(records, CONFIG_POWER4_MAX_BATTERIES, &count);
    if (err != ESP_OK) {
        printf("show batteries failed: %s\n", esp_err_to_name(err));
        free(records);
        return 1;
    }

    printf("batteries: %u/%u\n",
           static_cast<unsigned>(count),
           static_cast<unsigned>(battery_store_capacity()));
    if (count == 0) {
        printf("none observed\n");
        free(records);
        return 0;
    }

    const int64_t now_us = esp_timer_get_time();
    printf("%-31s %9s %9s %7s %7s %6s %10s\n",
           "name",
           "voltage",
           "current",
           "soc",
           "temp",
           "cycles",
           "age_s");
    for (size_t i = 0; i < count; ++i) {
        uint32_t age_s = 0;
        if (now_us >= records[i].last_seen_us) {
            age_s = static_cast<uint32_t>((now_us - records[i].last_seen_us) / 1000000LL);
        }

        if (records[i].temperature_valid) {
            printf("%-31s %8.3fV %8.3fA %6.1f%% %6.1fC %6u %10" PRIu32 "\n",
                   records[i].name,
                   static_cast<double>(records[i].voltage_v),
                   static_cast<double>(records[i].current_a),
                   static_cast<double>(records[i].soc_percent),
                   static_cast<double>(records[i].temperature_c),
                   records[i].cycle_count,
                   age_s);
        } else {
            printf("%-31s %8.3fV %8.3fA %6.1f%% %7s %6u %10" PRIu32 "\n",
                   records[i].name,
                   static_cast<double>(records[i].voltage_v),
                   static_cast<double>(records[i].current_a),
                   static_cast<double>(records[i].soc_percent),
                   "-",
                   records[i].cycle_count,
                   age_s);
        }
    }

    free(records);
    return 0;
}

int show_debug_command(void)
{
    printf("debug:\n");
    printf("  ble_scanner=%s\n", battery_scanner_verbose_enabled() ? "on" : "off");
    return 0;
}

int show_board_command(void)
{
    const BoardConfig *board = board_config_get();
    if (board == nullptr) {
        printf("board: unavailable (%s)\n", board_config_error());
        return 1;
    }

    printf("board: profile=%s model=\"%s\" schema=%u\n",
           board->profile,
           board->model,
           board->schema_version);
    printf("relays: backend=%s count=%u active_level=%u channels=",
           relay_backend_name(board->relay.kind),
           board->relay.count,
           board->relay.active_level);
    for (uint8_t i = 0; i < board->relay.count; ++i) {
        printf("%s%d", i == 0 ? "" : ",", board->relay.channels[i]);
    }
    if (board->relay.kind == RelayBackendKind::Tca9554) {
        printf(" address=0x%02x", board->relay.i2c_address);
    }
    printf("\n");

    if (board->i2c.present) {
        printf("i2c: port=%d sda=%d scl=%d frequency=%" PRIu32 "\n",
               board->i2c.port,
               board->i2c.sda_gpio,
               board->i2c.scl_gpio,
               board->i2c.frequency_hz);
    } else {
        printf("i2c: none\n");
    }

    if (board->ethernet.kind == EthernetHardwareKind::W5500) {
        printf("ethernet: hardware=%s spi_host=%d mosi=%d miso=%d sclk=%d "
               "cs=%d interrupt=%d reset=%d phy_address=%u frequency=%" PRIu32 "\n",
               ethernet_hardware_name(board->ethernet.kind),
               board->ethernet.spi_host,
               board->ethernet.mosi_gpio,
               board->ethernet.miso_gpio,
               board->ethernet.sclk_gpio,
               board->ethernet.cs_gpio,
               board->ethernet.interrupt_gpio,
               board->ethernet.reset_gpio,
               board->ethernet.phy_address,
               board->ethernet.spi_frequency_hz);
    } else {
        printf("ethernet: none\n");
    }

    if (board->rtc.kind != RtcHardwareKind::None) {
        printf("rtc: hardware=%s address=0x%02x\n",
               rtc_hardware_name(board->rtc.kind),
               board->rtc.i2c_address);
    } else {
        printf("rtc: none\n");
    }

    if (board->digital_input.kind == DigitalInputBackendKind::Gpio) {
        printf("inputs: backend=%s count=%u active_level=%u pull=%s channels=",
               digital_input_backend_name(board->digital_input.kind),
               board->digital_input.count,
               board->digital_input.active_level,
               gpio_pull_name(board->digital_input.pull));
        for (uint8_t i = 0; i < board->digital_input.count; ++i) {
            printf("%s%d", i == 0 ? "" : ",", board->digital_input.channels[i]);
        }
        printf("\n");
    } else {
        printf("inputs: none\n");
    }
    return 0;
}

const char *format_ip(const esp_ip4_addr_t &address, char *buffer, size_t capacity)
{
    if (esp_ip4addr_ntoa(&address, buffer, static_cast<int>(capacity)) == nullptr) {
        strlcpy(buffer, "invalid", capacity);
    }
    return buffer;
}

int show_ethernet_command(void)
{
    EthernetStatus status = {};
    const esp_err_t err = ethernet_manager_get_status(&status);
    if (err != ESP_OK) {
        printf("show ethernet failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (!status.present) {
        printf("ethernet: not present\n");
        return 0;
    }
    if (!status.initialized) {
        printf("ethernet: unavailable (%s)\n", esp_err_to_name(status.last_error));
        return 1;
    }

    printf("ethernet: address=%s phy=%s link=%s",
           ethernet_address_mode_name(status.settings.address_mode),
           ethernet_phy_mode_name(status.settings.phy_mode),
           status.link_up ? "up" : "down");
    if (status.link_up) {
        printf(" speed=%uMbps duplex=%s",
               status.speed_mbps,
               status.full_duplex ? "full" : "half");
    }
    printf("\n");
    printf("mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
           status.mac[0],
           status.mac[1],
           status.mac[2],
           status.mac[3],
           status.mac[4],
           status.mac[5]);

    char ip[16] = {};
    char netmask[16] = {};
    char gateway[16] = {};
    char dns1[16] = {};
    char dns2[16] = {};
    if (status.settings.address_mode == EthernetAddressMode::Static) {
        printf("configured: ip=%s netmask=%s gateway=%s dns1=%s dns2=%s\n",
               format_ip(status.settings.ip, ip, sizeof(ip)),
               format_ip(status.settings.netmask, netmask, sizeof(netmask)),
               format_ip(status.settings.gateway, gateway, sizeof(gateway)),
               format_ip(status.settings.dns1, dns1, sizeof(dns1)),
               format_ip(status.settings.dns2, dns2, sizeof(dns2)));
    }
    printf("current: ip=%s netmask=%s gateway=%s dns1=%s dns2=%s\n",
           format_ip(status.current_ip.ip, ip, sizeof(ip)),
           format_ip(status.current_ip.netmask, netmask, sizeof(netmask)),
           format_ip(status.current_ip.gw, gateway, sizeof(gateway)),
           format_ip(status.current_dns1.ip.u_addr.ip4, dns1, sizeof(dns1)),
           format_ip(status.current_dns2.ip.u_addr.ip4, dns2, sizeof(dns2)));
    return 0;
}

int show_espnow_command(void)
{
    EspNowStatus *status = static_cast<EspNowStatus *>(calloc(1, sizeof(EspNowStatus)));
    if (status == nullptr) {
        printf("show espnow failed: out of memory\n");
        return 1;
    }
    const esp_err_t err = espnow_manager_get_status(status);
    if (err != ESP_OK) {
        printf("show espnow failed: %s\n", esp_err_to_name(err));
        free(status);
        return 1;
    }
    char station_mac[18] = {};
    espnow_protocol_format_mac(status->station_mac, station_mac);
    printf("espnow: name=%s channel=",
           status->settings.name[0] != '\0' ? status->settings.name : "not-configured");
    if (status->settings.channel == 0) {
        printf("off");
    } else {
        printf("%u", status->settings.channel);
    }
    printf(" rate=%s radio=%s station_mac=%s last_error=%s\n",
           espnow_rate_name(status->settings.rate),
           status->radio_enabled ? "enabled" : "disabled",
           station_mac,
           esp_err_to_name(status->last_error));

    if (status->settings.gateway_enabled) {
        esp_ip4_addr_t address = {};
        address.addr = status->settings.gateway_ipv4;
        char ip[16] = {};
        printf("gateway: %s:%u\n",
               format_ip(address, ip, sizeof(ip)),
               status->settings.gateway_port);
    } else {
        printf("gateway: none\n");
    }
    printf("peers: %u/%u\n",
           static_cast<unsigned>(status->settings.peer_count),
           static_cast<unsigned>(kEspNowMaxPeers));
    for (size_t i = 0; i < status->settings.peer_count; ++i) {
        char mac[18] = {};
        espnow_protocol_format_mac(status->settings.peers[i].mac, mac);
        printf("  %s %s\n", status->settings.peers[i].name, mac);
    }
    printf("stats: cycles=%" PRIu32 " cycle_drops=%" PRIu32
           " tx_queued=%" PRIu32 " tx_success=%" PRIu32
           " tx_failed=%" PRIu32 " tx_timeout=%" PRIu32 "\n",
           status->counters.report_cycles,
           status->counters.report_cycles_dropped,
           status->counters.tx_queued,
           status->counters.tx_success,
           status->counters.tx_failed,
           status->counters.tx_timeout);
    printf("       rx=%" PRIu32 " rx_drops=%" PRIu32
           " gateway_forwarded=%" PRIu32 " gateway_drops=%" PRIu32 "\n",
           status->counters.rx_received,
           status->counters.rx_dropped,
           status->counters.gateway_forwarded,
           status->counters.gateway_dropped);
    free(status);
    return 0;
}

const char *weekday_name(uint8_t weekday)
{
    static constexpr const char *kWeekdays[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
    };
    return weekday < 7 ? kWeekdays[weekday] : "invalid";
}

int show_time_command(void)
{
    TimeSnapshot snapshot = {};
    const esp_err_t err = time_manager_get_time(&snapshot);
    if (err != ESP_OK) {
        printf("show time failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (!snapshot.valid) {
        printf("time: not set\n");
        return 1;
    }

    printf("time: %04d-%02d-%02d %02d:%02d:%02d UTC %s\n",
           snapshot.utc.tm_year + 1900,
           snapshot.utc.tm_mon + 1,
           snapshot.utc.tm_mday,
           snapshot.utc.tm_hour,
           snapshot.utc.tm_min,
           snapshot.utc.tm_sec,
           weekday_name(static_cast<uint8_t>(snapshot.utc.tm_wday)));
    const int offset = snapshot.utc_offset_minutes;
    const int offset_magnitude = offset < 0 ? -offset : offset;
    printf("local: %04d-%02d-%02d %02d:%02d:%02d %s %s "
           "timezone=%s offset=%c%02d:%02d source=%s\n",
           snapshot.local.tm_year + 1900,
           snapshot.local.tm_mon + 1,
           snapshot.local.tm_mday,
           snapshot.local.tm_hour,
           snapshot.local.tm_min,
           snapshot.local.tm_sec,
           snapshot.abbreviation,
           weekday_name(static_cast<uint8_t>(snapshot.local.tm_wday)),
           snapshot.timezone_name,
           offset < 0 ? '-' : '+',
           offset_magnitude / 60,
           offset_magnitude % 60,
           time_source_name(snapshot.source));
    return 0;
}

int show_timezone_command(void)
{
    TimeManagerStatus status = {};
    const esp_err_t err = time_manager_get_status(&status);
    if (err != ESP_OK) {
        printf("show timezone failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("timezone: %s posix=%s current=%s\n",
           status.timezone_name,
           status.posix_timezone,
           status.current_abbreviation[0] != '\0'
               ? status.current_abbreviation
               : "clock-not-set");
    printf("sntp: %s server=%s\n",
           status.sntp_started ? "started" : "stopped",
           time_manager_sntp_server());
    return 0;
}

int show_timezones_command(void)
{
    printf("%-20s %-5s %s\n", "name", "short", "POSIX rule");
    for (size_t index = 0; index < time_manager_timezone_count(); ++index) {
        const TimezoneInfo *timezone = time_manager_timezone_at(index);
        if (timezone == nullptr) {
            printf("show timezones failed: invalid table entry\n");
            return 1;
        }
        printf("%-20s %-5s %s\n",
               timezone->human_name,
               timezone->short_name,
               timezone->posix_rule);
    }
    return 0;
}

int show_inputs_command(void)
{
    InputManagerStatus manager = {};
    esp_err_t err = input_manager_get_status(&manager);
    if (err != ESP_OK) {
        printf("show inputs failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (!manager.present) {
        printf("inputs: not present\n");
        return 0;
    }
    if (!manager.initialized) {
        printf("inputs: unavailable (%s)\n", esp_err_to_name(manager.initialization_result));
        return 1;
    }

    printf("inputs: count=%u\n", manager.count);
    int result = 0;
    for (uint8_t input = 1; input <= manager.count; ++input) {
        InputStatus status = {};
        err = input_manager_query(input, &status);
        if (err != ESP_OK) {
            printf("input %u: error=%s\n", input, esp_err_to_name(err));
            result = 1;
            continue;
        }
        printf("input %u: backend=%s gpio=%d active_level=%u level=%d state=%s\n",
               status.input,
               digital_input_backend_name(status.backend),
               status.gpio_pin,
               status.active_level,
               status.level,
               status.on ? "on" : "off");
    }
    return result;
}

int show_command(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_show_usage();
        return argc < 2 ? 1 : 0;
    }

    if (strcmp(argv[1], "batteries") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return show_batteries_command();
    }

    if (strcmp(argv[1], "banks") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return bank_show_command();
    }

    if (strcmp(argv[1], "ble") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return ble_show_command();
    }

    if (strcmp(argv[1], "board") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return show_board_command();
    }

    if (strcmp(argv[1], "debug") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return show_debug_command();
    }

    if (strcmp(argv[1], "ethernet") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return show_ethernet_command();
    }

    if (strcmp(argv[1], "espnow") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return show_espnow_command();
    }

    if (strcmp(argv[1], "password") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        if (g_command_source == Power4CommandSource::Tcp) {
            printf("show password denied: serial console only\n");
            return 1;
        }

        char password[129] = {};
        bool configured = false;
        const esp_err_t err =
            network_console_password_get(password, sizeof(password), &configured);
        if (err != ESP_OK) {
            printf("show password failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("password: %s\n", configured ? password : "not configured");
        memset(password, 0, sizeof(password));
        return 0;
    }

    if (strcmp(argv[1], "inputs") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return show_inputs_command();
    }

    if (strcmp(argv[1], "policy") == 0) {
        if (argc == 2) {
            return print_policy_slot(PolicySlot::Active);
        }
        if (argc == 3 && strcmp(argv[2], "staged") == 0) {
            return print_policy_slot(PolicySlot::Staged);
        }
        if (argc == 3 && strcmp(argv[2], "parameters") == 0) {
            return print_config_flags();
        }

        print_show_usage();
        return 1;
    }

    if (strcmp(argv[1], "logs") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return print_buffered_logs();
    }

    if (strcmp(argv[1], "relays") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return print_all_relays();
    }

    if (strcmp(argv[1], "system") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return system_command(argc, argv);
    }

    if (strcmp(argv[1], "time") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return show_time_command();
    }

    if (strcmp(argv[1], "timezone") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return show_timezone_command();
    }

    if (strcmp(argv[1], "timezones") == 0) {
        if (argc != 2) {
            print_show_usage();
            return 1;
        }
        return show_timezones_command();
    }

    print_show_usage();
    return 1;
}

void format_ble_addr(const ble_addr_t &addr, char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0) {
        return;
    }

    snprintf(buffer,
             buffer_size,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             addr.val[5],
             addr.val[4],
             addr.val[3],
             addr.val[2],
             addr.val[1],
             addr.val[0]);
}

const char *ble_addr_type_name(uint8_t type)
{
    switch (type) {
    case BLE_ADDR_PUBLIC:
        return "public";
    case BLE_ADDR_RANDOM:
        return "random";
    case BLE_ADDR_PUBLIC_ID:
        return "public-id";
    case BLE_ADDR_RANDOM_ID:
        return "random-id";
    default:
        return "unknown";
    }
}

const char *ble_role_name(uint8_t role)
{
    switch (role) {
    case BLE_GAP_ROLE_MASTER:
        return "master";
    case BLE_GAP_ROLE_SLAVE:
        return "slave";
    default:
        return "unknown";
    }
}

struct BleConnectionList {
    uint16_t handles[CONFIG_BT_NIMBLE_MAX_CONNECTIONS] = {};
    size_t count = 0;
    bool truncated = false;
};

int collect_ble_connection_handle(uint16_t conn_handle, void *arg)
{
    BleConnectionList *connections = static_cast<BleConnectionList *>(arg);
    if (connections == nullptr) {
        return 0;
    }

    if (connections->count >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
        connections->truncated = true;
        return 0;
    }

    connections->handles[connections->count] = conn_handle;
    ++connections->count;
    return 0;
}

int ble_show_command(void)
{
    BleConnectionList connections = {};
    ble_gap_conn_foreach_handle(collect_ble_connection_handle, &connections);

    printf("ble:\n");
    printf("  connections=%u/%u%s\n",
           static_cast<unsigned>(connections.count),
           static_cast<unsigned>(CONFIG_BT_NIMBLE_MAX_CONNECTIONS),
           connections.truncated ? " (truncated)" : "");
    printf("  advertising=%s\n", ble_gap_adv_active() ? "active" : "idle");
    printf("  scanning=%s\n", ble_gap_disc_active() ? "active" : "idle");
    printf("  connecting=%s\n", ble_gap_conn_active() ? "active" : "idle");

    for (size_t i = 0; i < connections.count; ++i) {
        ble_gap_conn_desc desc = {};
        const int rc = ble_gap_conn_find(connections.handles[i], &desc);
        if (rc != 0) {
            printf("  handle=%u state=error:%d\n",
                   static_cast<unsigned>(connections.handles[i]),
                   rc);
            continue;
        }

        char peer_ota[18] = {};
        char peer_id[18] = {};
        format_ble_addr(desc.peer_ota_addr, peer_ota, sizeof(peer_ota));
        format_ble_addr(desc.peer_id_addr, peer_id, sizeof(peer_id));
        printf("  handle=%u role=%s peer_ota=%s(%s) peer_id=%s(%s) interval=%u latency=%u timeout=%u\n",
               static_cast<unsigned>(desc.conn_handle),
               ble_role_name(desc.role),
               peer_ota,
               ble_addr_type_name(desc.peer_ota_addr.type),
               peer_id,
               ble_addr_type_name(desc.peer_id_addr.type),
               static_cast<unsigned>(desc.conn_itvl),
               static_cast<unsigned>(desc.conn_latency),
               static_cast<unsigned>(desc.supervision_timeout));
    }

    return 0;
}

int bank_show_command(void)
{
    BatteryBankList *banks = static_cast<BatteryBankList *>(malloc(sizeof(BatteryBankList)));
    if (banks == nullptr) {
        printf("bank show failed: out of memory\n");
        return 1;
    }

    const esp_err_t err = battery_bank_list(banks);
    if (err != ESP_OK) {
        printf("bank show failed: %s\n", esp_err_to_name(err));
        free(banks);
        return 1;
    }

    printf("banks: %u/%u\n",
           static_cast<unsigned>(banks->count),
           static_cast<unsigned>(kBatteryBankMaxBanks));
    if (banks->count == 0) {
        printf("none configured\n");
        free(banks);
        return 0;
    }

    for (size_t i = 0; i < banks->count; ++i) {
        const BatteryBankDefinition &bank = banks->banks[i];
        BatteryBankState state = {};
        const esp_err_t state_err = battery_bank_get_state(bank.name, &state);

        printf("%s:", bank.name);
        for (size_t j = 0; j < bank.battery_count; ++j) {
            printf(" %s", bank.batteries[j]);
        }

        if (state_err != ESP_OK) {
            printf(" state=error:%s\n", esp_err_to_name(state_err));
        } else if (!state.ready) {
            printf(" state=not-ready\n");
        } else {
            printf(" voltage=%.3fV current=%.3fA soc=%.1f%%\n",
                   static_cast<double>(state.voltage_v),
                   static_cast<double>(state.current_a),
                   static_cast<double>(state.soc_percent));
        }
    }

    free(banks);
    return 0;
}

bool parse_on_off(const char *text, bool *value)
{
    if (text == nullptr || value == nullptr) {
        return false;
    }
    if (strcmp(text, "on") == 0 || strcmp(text, "true") == 0) {
        *value = true;
        return true;
    }
    if (strcmp(text, "off") == 0 || strcmp(text, "false") == 0) {
        *value = false;
        return true;
    }
    return false;
}

bool parse_lifetime(const char *text, uint32_t *seconds)
{
    if (text == nullptr || seconds == nullptr || text[0] == '\0') {
        return false;
    }

    char stripped[16] = {};
    const size_t length = strlen(text);
    if (length >= sizeof(stripped)) {
        return false;
    }
    strlcpy(stripped, text, sizeof(stripped));
    if (stripped[length - 1] == 's') {
        stripped[length - 1] = '\0';
    }

    return parse_u32(stripped, seconds) && *seconds > 0;
}

char *trim_token(char *text)
{
    while (*text == ' ') {
        ++text;
    }
    char *end = text + strlen(text);
    while (end > text && end[-1] == ' ') {
        --end;
    }
    *end = '\0';
    return text;
}

void print_set_usage(void)
{
    printf("usage:\n");
    printf("  set debug ble_scanner on\n");
    printf("  set debug ble_scanner off\n");
    printf("  set ethernet dhcp\n");
    printf("  set ethernet static <ip> <netmask> <gateway> [dns1] [dns2]\n");
    printf("  set ethernet phy auto|10-half|10-full|100-half|100-full\n");
    printf("  set espnow name <name>\n");
    printf("  set espnow peer <name> <mac|none>\n");
    printf("  set espnow channel <1-14|off>\n");
    printf("  set espnow rate <auto|lr-500|lr-250>\n");
    printf("  set espnow gateway <ipv4> <port>\n");
    printf("  set espnow gateway none\n");
    printf("  set password [password]\n");
    printf("  set relay <relay> on [seconds]\n");
    printf("  set relay <relay> force-on\n");
    printf("  set relay <relay> force-off\n");
    printf("  set relay <relay> clear-force\n");
    printf("  set time YYYY-MM-DD HH:MM:SS  (UTC)\n");
    printf("  set timezone <name>  (see 'show timezones')\n");
}

bool parse_rtc_datetime(const char *date_text, const char *time_text, RtcDateTime *value)
{
    if (date_text == nullptr || time_text == nullptr || value == nullptr) {
        return false;
    }
    if (strlen(date_text) != 10 ||
        date_text[4] != '-' ||
        date_text[7] != '-' ||
        strlen(time_text) != 8 ||
        time_text[2] != ':' ||
        time_text[5] != ':') {
        return false;
    }

    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    int date_chars = 0;
    int time_chars = 0;
    if (sscanf(date_text,
               "%u-%u-%u%n",
               &year,
               &month,
               &day,
               &date_chars) != 3 ||
        date_text[date_chars] != '\0' ||
        sscanf(time_text,
               "%u:%u:%u%n",
               &hour,
               &minute,
               &second,
               &time_chars) != 3 ||
        time_text[time_chars] != '\0' ||
        year > UINT16_MAX ||
        month > UINT8_MAX ||
        day > UINT8_MAX ||
        hour > UINT8_MAX ||
        minute > UINT8_MAX ||
        second > UINT8_MAX) {
        return false;
    }

    RtcDateTime parsed = {};
    parsed.year = static_cast<uint16_t>(year);
    parsed.month = static_cast<uint8_t>(month);
    parsed.day = static_cast<uint8_t>(day);
    parsed.hour = static_cast<uint8_t>(hour);
    parsed.minute = static_cast<uint8_t>(minute);
    parsed.second = static_cast<uint8_t>(second);
    if (!rtc_datetime_valid(parsed)) {
        return false;
    }
    parsed.weekday = rtc_weekday(parsed.year, parsed.month, parsed.day);
    *value = parsed;
    return true;
}

int set_command(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_set_usage();
        return argc < 2 ? 1 : 0;
    }

    if (strcmp(argv[1], "debug") == 0) {
        bool enabled = false;
        if (argc != 4 || strcmp(argv[2], "ble_scanner") != 0 || !parse_on_off(argv[3], &enabled)) {
            print_set_usage();
            return 1;
        }

        battery_scanner_set_verbose(enabled);
        printf("debug ble_scanner %s\n", battery_scanner_verbose_enabled() ? "on" : "off");
        return 0;
    }

    if (strcmp(argv[1], "password") == 0) {
        if (g_command_source == Power4CommandSource::Tcp) {
            printf("set password denied: serial console only\n");
            return 1;
        }
        if (argc < 2 || argc > 3) {
            print_set_usage();
            return 1;
        }

        char generated[129] = {};
        const esp_err_t err =
            argc == 2 ? network_console_password_generate(generated, sizeof(generated))
                      : network_console_password_set(argv[2]);
        if (err != ESP_OK) {
            printf("set password failed: %s\n", esp_err_to_name(err));
            memset(generated, 0, sizeof(generated));
            return 1;
        }
        printf("password: %s\n", argc == 2 ? generated : argv[2]);
        memset(generated, 0, sizeof(generated));
        return 0;
    }

    if (strcmp(argv[1], "ethernet") == 0) {
        if (g_command_source == Power4CommandSource::Tcp) {
            printf("set ethernet denied: serial console only\n");
            return 1;
        }
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (argc == 3 && strcmp(argv[2], "dhcp") == 0) {
            err = ethernet_manager_set_dhcp();
        } else if (argc >= 6 && argc <= 8 && strcmp(argv[2], "static") == 0) {
            err = ethernet_manager_set_static(argv[3],
                                              argv[4],
                                              argv[5],
                                              argc >= 7 ? argv[6] : nullptr,
                                              argc >= 8 ? argv[7] : nullptr);
        } else if (argc == 4 && strcmp(argv[2], "phy") == 0) {
            EthernetPhyMode mode = EthernetPhyMode::Auto;
            if (!ethernet_phy_mode_parse(argv[3], &mode)) {
                print_set_usage();
                return 1;
            }
            err = ethernet_manager_set_phy(mode);
        } else {
            print_set_usage();
            return 1;
        }

        if (err != ESP_OK) {
            printf("set ethernet failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        return show_ethernet_command();
    }

    if (strcmp(argv[1], "espnow") == 0) {
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (argc == 4 && strcmp(argv[2], "name") == 0) {
            err = espnow_manager_set_name(argv[3]);
        } else if (argc == 5 && strcmp(argv[2], "peer") == 0) {
            err = espnow_manager_set_peer(argv[3], argv[4]);
        } else if (argc == 4 && strcmp(argv[2], "channel") == 0) {
            if (strcmp(argv[3], "off") == 0) {
                err = espnow_manager_set_channel(0);
            } else {
                uint32_t channel = 0;
                if (!parse_u32(argv[3], &channel) || channel == 0 || channel > 14) {
                    print_set_usage();
                    return 1;
                }
                err = espnow_manager_set_channel(static_cast<uint8_t>(channel));
            }
        } else if (argc == 4 && strcmp(argv[2], "rate") == 0) {
            EspNowRate rate = EspNowRate::Auto;
            if (!espnow_rate_parse(argv[3], &rate)) {
                print_set_usage();
                return 1;
            }
            err = espnow_manager_set_rate(rate);
        } else if (argc == 4 && strcmp(argv[2], "gateway") == 0 &&
                   strcmp(argv[3], "none") == 0) {
            err = espnow_manager_clear_gateway();
        } else if (argc == 5 && strcmp(argv[2], "gateway") == 0) {
            uint32_t port = 0;
            if (!parse_u32(argv[4], &port) || port == 0 || port > UINT16_MAX) {
                print_set_usage();
                return 1;
            }
            err = espnow_manager_set_gateway(argv[3], static_cast<uint16_t>(port));
        } else {
            print_set_usage();
            return 1;
        }
        if (err != ESP_OK) {
            printf("set espnow failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        return show_espnow_command();
    }

    if (strcmp(argv[1], "relay") == 0) {
        uint8_t relay = 0;
        if ((argc < 4 || argc > 5) || !parse_relay(argv[2], &relay)) {
            print_set_usage();
            return 1;
        }

        if (strcmp(argv[3], "on") == 0) {
            uint32_t seconds = 300;
            if (argc == 5 && (!parse_u32(argv[4], &seconds) || seconds == 0)) {
                print_set_usage();
                return 1;
            }
            const esp_err_t err = relay_manager_on_for(relay, seconds);
            if (err != ESP_OK) {
                printf("set relay failed: %s\n", esp_err_to_name(err));
                return 1;
            }
            return query_and_print_relay(relay);
        }

        if (strcmp(argv[3], "clear-force") == 0) {
            if (argc != 4) {
                print_set_usage();
                return 1;
            }
            const esp_err_t err = relay_manager_clear_force(relay);
            if (err != ESP_OK) {
                printf("set relay failed: %s\n", esp_err_to_name(err));
                return 1;
            }
            return query_and_print_relay(relay);
        }

        if (strcmp(argv[3], "force-on") == 0) {
            if (argc != 4) {
                print_set_usage();
                return 1;
            }
            const esp_err_t err = relay_manager_force_on(relay);
            if (err != ESP_OK) {
                printf("set relay failed: %s\n", esp_err_to_name(err));
                return 1;
            }
            return query_and_print_relay(relay);
        }

        if (strcmp(argv[3], "force-off") == 0) {
            if (argc != 4) {
                print_set_usage();
                return 1;
            }
            const esp_err_t err = relay_manager_force_off(relay);
            if (err != ESP_OK) {
                printf("set relay failed: %s\n", esp_err_to_name(err));
                return 1;
            }
            return query_and_print_relay(relay);
        }

        print_set_usage();
        return 1;
    }

    if (strcmp(argv[1], "time") == 0) {
        RtcDateTime value = {};
        if (argc != 4 || !parse_rtc_datetime(argv[2], argv[3], &value)) {
            print_set_usage();
            return 1;
        }
        const esp_err_t err = time_manager_set_utc(value);
        if (err != ESP_OK) {
            printf("set time failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        return show_time_command();
    }

    if (strcmp(argv[1], "timezone") == 0) {
        if (argc != 3) {
            print_set_usage();
            return 1;
        }
        const esp_err_t err = time_manager_set_timezone(argv[2]);
        if (err != ESP_OK) {
            printf("set timezone failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        return show_timezone_command();
    }

    print_set_usage();
    return 1;
}

void print_define_usage(void)
{
    printf("usage:\n");
    printf("  define bank <name> <battery> [battery...]\n");
    printf("  define policy <name>=true|false|<number> [<seconds>s]\n");
}

int define_command(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_define_usage();
        return argc < 2 ? 1 : 0;
    }

    if (strcmp(argv[1], "bank") == 0) {
        if (argc < 5 || !battery_bank_valid_name(argv[2])) {
            print_define_usage();
            return 1;
        }

        const size_t battery_count = static_cast<size_t>(argc - 3);
        const char *battery_names[CONFIG_POWER4_MAX_BATTERIES] = {};
        if (battery_count > CONFIG_POWER4_MAX_BATTERIES) {
            print_define_usage();
            return 1;
        }
        for (size_t i = 0; i < battery_count; ++i) {
            battery_names[i] = argv[i + 3];
        }

        const esp_err_t err = battery_bank_create(argv[2], battery_names, battery_count);
        if (err != ESP_OK) {
            printf("define bank failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        printf("defined bank %s:", argv[2]);
        for (size_t i = 0; i < battery_count; ++i) {
            printf(" %s", battery_names[i]);
        }
        printf("\n");
        return 0;
    }

    if (strcmp(argv[1], "policy") == 0) {
        // Rejoin the arguments so "name=40", "name =40", and "name = 40"
        // all parse the same; the console splits arguments on spaces.
        char assignment[80] = {};
        size_t used = 0;
        for (int i = 2; i < argc; ++i) {
            const size_t length = strlen(argv[i]);
            if (used + length + 2 > sizeof(assignment)) {
                print_define_usage();
                return 1;
            }
            if (used > 0) {
                assignment[used++] = ' ';
            }
            memcpy(&assignment[used], argv[i], length);
            used += length;
        }
        assignment[used] = '\0';

        char *equals = strchr(assignment, '=');
        if (equals == nullptr) {
            print_define_usage();
            return 1;
        }
        *equals = '\0';
        const char *name = trim_token(assignment);

        char *save = nullptr;
        char *value_text = strtok_r(equals + 1, " ", &save);
        char *lifetime_text = strtok_r(nullptr, " ", &save);
        if (value_text == nullptr || strtok_r(nullptr, " ", &save) != nullptr) {
            print_define_usage();
            return 1;
        }

        if (!config_flags_valid_name(name)) {
            printf("cannot set policy parameter '%s': names are 1-15 characters of "
                   "letters, digits, '_', '-'\n",
                   name);
            return 1;
        }

        uint32_t lifetime_s = 0;
        if (lifetime_text != nullptr && !parse_lifetime(lifetime_text, &lifetime_s)) {
            print_define_usage();
            return 1;
        }

        bool bool_value = false;
        ConfigNumber number = {};
        esp_err_t err = ESP_OK;
        const char *echo_value = value_text;
        if (parse_on_off(value_text, &bool_value)) {
            err = config_flags_set(name, bool_value, lifetime_s);
            echo_value = bool_value ? "true" : "false";
        } else if (config_number_parse(value_text, &number)) {
            err = config_flags_set_number(name, value_text, lifetime_s);
        } else {
            printf("cannot set policy parameter '%s': value must be true, false, or a number\n",
                   name);
            return 1;
        }

        if (err != ESP_OK) {
            printf("define policy %s failed: %s\n", name, esp_err_to_name(err));
            return 1;
        }

        if (lifetime_s > 0) {
            printf("policy %s=%s for %us\n", name, echo_value, static_cast<unsigned>(lifetime_s));
        } else {
            printf("policy %s=%s\n", name, echo_value);
        }
        return 0;
    }

    print_define_usage();
    return 1;
}

void print_remove_usage(void)
{
    printf("usage:\n");
    printf("  remove bank <name>\n");
    printf("  remove policy <name>\n");
}

int remove_command(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_remove_usage();
        return argc < 2 ? 1 : 0;
    }

    if (strcmp(argv[1], "bank") == 0) {
        if (argc != 3 || !battery_bank_valid_name(argv[2])) {
            print_remove_usage();
            return 1;
        }

        const esp_err_t err = battery_bank_remove(argv[2]);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("remove bank %s failed: not found\n", argv[2]);
            return 1;
        }
        if (err != ESP_OK) {
            printf("remove bank %s failed: %s\n", argv[2], esp_err_to_name(err));
            return 1;
        }

        printf("removed bank %s\n", argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "policy") == 0) {
        if (argc != 3) {
            print_remove_usage();
            return 1;
        }
        if (!config_flags_valid_name(argv[2])) {
            printf("no such policy parameter '%s': names are 1-15 characters of "
                   "letters, digits, '_', '-'\n",
                   argv[2]);
            return 1;
        }

        ConfigFlagType type = ConfigFlagType::NotSet;
        esp_err_t err = config_flags_type(argv[2], &type);
        if (err != ESP_OK) {
            printf("remove policy %s failed: %s\n", argv[2], esp_err_to_name(err));
            return 1;
        }

        err = config_flags_unset(argv[2]);
        if (err != ESP_OK) {
            printf("remove policy %s failed: %s\n", argv[2], esp_err_to_name(err));
            return 1;
        }

        printf("removed policy %s%s\n",
               argv[2],
               type != ConfigFlagType::NotSet ? "" : " (was not defined)");
        return 0;
    }

    print_remove_usage();
    return 1;
}

bool append_json(char *buffer, size_t capacity, size_t *used, const char *format, ...)
{
    if (buffer == nullptr || used == nullptr || *used >= capacity) {
        return false;
    }

    va_list args;
    va_start(args, format);
    const int written = vsnprintf(buffer + *used, capacity - *used, format, args);
    va_end(args);

    if (written < 0 || static_cast<size_t>(written) >= capacity - *used) {
        return false;
    }

    *used += static_cast<size_t>(written);
    return true;
}

bool append_json_string(char *buffer, size_t capacity, size_t *used, const char *text)
{
    if (!append_json(buffer, capacity, used, "\"")) {
        return false;
    }

    for (const char *cursor = text; cursor != nullptr && *cursor != '\0'; ++cursor) {
        const unsigned char ch = static_cast<unsigned char>(*cursor);
        if (ch == '"' || ch == '\\') {
            if (!append_json(buffer, capacity, used, "\\%c", ch)) {
                return false;
            }
        } else if (ch >= ' ' && ch <= '~') {
            if (!append_json(buffer, capacity, used, "%c", ch)) {
                return false;
            }
        } else if (!append_json(buffer, capacity, used, "\\u%04x", ch)) {
            return false;
        }
    }

    return append_json(buffer, capacity, used, "\"");
}

int print_state_report(StateReportKind kind)
{
    char *json = nullptr;
    size_t length = 0;
    esp_err_t err = state_report_build(kind, &json, &length);
    (void)length;
    if (err == ESP_OK) {
        err = json_output_print(json);
    }
    free(json);
    if (err != ESP_OK) {
        printf("report %s failed: %s\n",
               state_report_command_name(kind),
               esp_err_to_name(err));
        return 1;
    }
    return 0;
}

int report_relays_command(void)
{
    return print_state_report(StateReportKind::Relays);
}

int report_inputs_command(void)
{
    return print_state_report(StateReportKind::Inputs);
}

int report_parameters_command(void)
{
    ConfigFlagList *parameters =
        static_cast<ConfigFlagList *>(malloc(sizeof(ConfigFlagList)));
    if (parameters == nullptr) {
        printf("report parameters failed: out of memory\n");
        return 1;
    }

    esp_err_t err = config_flags_list(parameters);
    if (err != ESP_OK) {
        printf("report parameters failed: %s\n", esp_err_to_name(err));
        free(parameters);
        return 1;
    }

    const size_t capacity =
        kParameterStateJsonBaseBytes +
        (kConfigFlagListMax * kParameterStateJsonBytesPerParameter);
    char *json = static_cast<char *>(malloc(capacity));
    if (json == nullptr) {
        printf("report parameters failed: out of memory\n");
        free(parameters);
        return 1;
    }

    size_t order[kConfigFlagListMax] = {};
    config_flag_sort_order(*parameters, order);

    size_t used = 0;
    bool ok = append_json(json,
                          capacity,
                          &used,
                          "{\"type\":\"policy_parameters\",\"capacity\":%u,\"count\":%u,"
                          "\"truncated\":%s,\"parameters\":[",
                          static_cast<unsigned>(kConfigFlagListMax),
                          static_cast<unsigned>(parameters->count),
                          parameters->truncated ? "true" : "false");

    for (size_t i = 0; ok && i < parameters->count; ++i) {
        const size_t k = order[i];
        ok = append_json(json, capacity, &used, "%s{\"name\":", i == 0 ? "" : ",");
        ok = ok && append_json_string(json, capacity, &used, parameters->names[k]);
        ok = ok && append_json(json,
                               capacity,
                               &used,
                               ",\"value_type\":\"%s\",\"value\":",
                               config_flag_type_name(parameters->types[k]));

        if (ok && parameters->types[k] == ConfigFlagType::Boolean) {
            ok = append_json(json, capacity, &used, "%s", parameters->values[k]);
        } else if (ok && parameters->types[k] == ConfigFlagType::Number) {
            ConfigNumber number = {};
            if (!config_number_parse(parameters->values[k], &number)) {
                ok = false;
            } else if (number.is_integer) {
                ok = append_json(json, capacity, &used, "%lld", number.integer);
            } else {
                ok = append_json(json, capacity, &used, "%.17g", number.value);
            }
        } else if (ok) {
            ok = append_json(json, capacity, &used, "null");
        }

        ok = ok && append_json(json, capacity, &used, ",\"value_text\":");
        ok = ok && append_json_string(json, capacity, &used, parameters->values[k]);
        ok = ok && append_json(json,
                               capacity,
                               &used,
                               ",\"lifetime_s\":%" PRIu32 ",\"remaining_s\":%" PRIu32 "}",
                               parameters->lifetime_s[k],
                               parameters->remaining_s[k]);
    }

    ok = ok && append_json(json, capacity, &used, "]}");
    free(parameters);
    if (!ok) {
        printf("report parameters failed: JSON buffer too small or invalid parameter value\n");
        free(json);
        return 1;
    }

    err = json_output_print(json);
    free(json);
    if (err != ESP_OK) {
        printf("report parameters failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

int report_batteries_command(void)
{
    return print_state_report(StateReportKind::Batteries);
}

int report_banks_command(void)
{
    return print_state_report(StateReportKind::Banks);
}

int report_logs_command(void)
{
    char *text = static_cast<char *>(malloc(kLogBufferBytes + 1));
    if (text == nullptr) {
        printf("report logs failed: out of memory\n");
        return 1;
    }

    const size_t length = log_buffer_snapshot(text, kLogBufferBytes);
    text[length] = '\0';

    // Escaped size varies per character, so measure before allocating.
    size_t escaped = 0;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == '"' || ch == '\\') {
            escaped += 2;
        } else if (ch >= ' ' && ch <= '~') {
            escaped += 1;
        } else {
            escaped += 6;  // \uXXXX
        }
    }

    const size_t capacity = kLogsJsonBaseBytes + escaped;
    char *json = static_cast<char *>(malloc(capacity));
    if (json == nullptr) {
        printf("report logs failed: out of memory\n");
        free(text);
        return 1;
    }

    size_t used = 0;
    bool ok = append_json(json,
                          capacity,
                          &used,
                          "{\"type\":\"logs\",\"length\":%u,\"text\":",
                          static_cast<unsigned>(length));
    ok = ok && append_json_string(json, capacity, &used, text);
    ok = ok && append_json(json, capacity, &used, "}");
    free(text);

    if (!ok) {
        printf("report logs failed: JSON buffer too small\n");
        free(json);
        return 1;
    }

    const esp_err_t err = json_output_print(json);
    free(json);
    if (err != ESP_OK) {
        printf("report logs failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    return 0;
}

void print_report_usage(void)
{
    printf("usage:\n");
    printf("  report banks\n");
    printf("  report batteries\n");
    printf("  report inputs\n");
    printf("  report logs\n");
    printf("  report parameters\n");
    printf("  report relays\n");
}

int report_command(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_report_usage();
        return argc < 2 ? 1 : 0;
    }

    if (strcmp(argv[1], "relays") == 0) {
        if (argc != 2) {
            print_report_usage();
            return 1;
        }
        return report_relays_command();
    }

    if (strcmp(argv[1], "inputs") == 0) {
        if (argc != 2) {
            print_report_usage();
            return 1;
        }
        return report_inputs_command();
    }

    if (strcmp(argv[1], "parameters") == 0) {
        if (argc != 2) {
            print_report_usage();
            return 1;
        }
        return report_parameters_command();
    }

    if (strcmp(argv[1], "banks") == 0) {
        if (argc != 2) {
            print_report_usage();
            return 1;
        }
        return report_banks_command();
    }

    if (strcmp(argv[1], "batteries") == 0) {
        if (argc != 2) {
            print_report_usage();
            return 1;
        }
        return report_batteries_command();
    }

    if (strcmp(argv[1], "logs") == 0) {
        if (argc != 2) {
            print_report_usage();
            return 1;
        }
        return report_logs_command();
    }

    print_report_usage();
    return 1;
}

void print_policy_usage(void)
{
    printf("usage:\n");
    printf("  policy upload <sha1-hex>\n");
    printf("  policy accept\n");
}

bool is_base64_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '+' || c == '/' || c == '=';
}

size_t strip_line_end(char *line)
{
    size_t length = strlen(line);
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[--length] = '\0';
    }
    return length;
}

bool append_base64_line(char *encoded, size_t capacity, size_t *used, const char *line)
{
    const size_t line_length = strlen(line);
    if (*used + line_length > capacity) {
        return false;
    }

    memcpy(encoded + *used, line, line_length);
    *used += line_length;
    encoded[*used] = '\0';
    return true;
}

int policy_upload_staged(const uint8_t expected_sha1[kChecksumSha1Bytes])
{
    char *encoded = static_cast<char *>(malloc(kPolicyUploadMaxEncodedBytes + 1));
    uint8_t *decoded = static_cast<uint8_t *>(malloc(kPolicyUploadMaxDecodedBytes + 1));
    if (encoded == nullptr || decoded == nullptr) {
        printf("policy upload failed: out of memory\n");
        free(encoded);
        free(decoded);
        return 1;
    }

    encoded[0] = '\0';
    size_t encoded_length = 0;
    char line[kPolicyUploadLineBytes] = {};

    printf("paste base64 policy; blank or invalid line ends upload\n");
    while (fgets(line, sizeof(line), stdin) != nullptr) {
        const size_t line_length = strip_line_end(line);
        if (line_length == 0) {
            printf("upload input ended: blank line\n");
            break;
        }

        bool valid = true;
        for (size_t i = 0; i < line_length; ++i) {
            if (!is_base64_char(line[i])) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            printf("upload input ended: invalid line\n");
            break;
        }

        printf("%s\n", line);
        if (!append_base64_line(encoded,
                                kPolicyUploadMaxEncodedBytes,
                                &encoded_length,
                                line)) {
            printf("policy upload failed: encoded input too large\n");
            free(encoded);
            free(decoded);
            return 1;
        }
    }

    size_t decoded_length = 0;
    const int decode_result = mbedtls_base64_decode(decoded,
                                                   kPolicyUploadMaxDecodedBytes,
                                                   &decoded_length,
                                                   reinterpret_cast<const unsigned char *>(encoded),
                                                   encoded_length);
    if (decode_result != 0) {
        printf("policy upload failed: invalid base64 (%d)\n", decode_result);
        free(encoded);
        free(decoded);
        return 1;
    }

    decoded[decoded_length] = '\0';
    uint8_t actual_sha1[kChecksumSha1Bytes] = {};
    esp_err_t err = checksum_sha1(decoded, decoded_length, actual_sha1);
    if (err != ESP_OK) {
        printf("policy upload failed: checksum calculation: %s\n", esp_err_to_name(err));
        free(encoded);
        free(decoded);
        return 1;
    }

    char expected_hex[kChecksumSha1HexChars + 1] = {};
    char actual_hex[kChecksumSha1HexChars + 1] = {};
    checksum_bytes_to_hex(expected_sha1, kChecksumSha1Bytes, expected_hex, sizeof(expected_hex));
    checksum_bytes_to_hex(actual_sha1, kChecksumSha1Bytes, actual_hex, sizeof(actual_hex));

    if (memcmp(actual_sha1, expected_sha1, kChecksumSha1Bytes) != 0) {
        printf("policy upload failed: checksum mismatch expected=%s actual=%s\n",
               expected_hex,
               actual_hex);
        free(encoded);
        free(decoded);
        return 1;
    }

    err = policy_storage_write_staged(decoded, decoded_length);
    if (err != ESP_OK) {
        printf("policy upload failed: %s\n", esp_err_to_name(err));
        free(encoded);
        free(decoded);
        return 1;
    }

    printf("uploaded staged configuration: %u bytes sha1=%s\n",
           static_cast<unsigned>(decoded_length),
           actual_hex);
    ESP_LOGI(kTag,
             "policy staged: %u bytes sha1=%s",
             static_cast<unsigned>(decoded_length),
             actual_hex);

    free(encoded);
    free(decoded);
    return 0;
}

int print_policy_slot(PolicySlot slot)
{
    char *contents = nullptr;
    size_t length = 0;
    const esp_err_t err = policy_storage_read_alloc(slot, &contents, &length);
    if (err != ESP_OK) {
        printf("show policy %s failed: %s\n",
               policy_storage_slot_name(slot),
               esp_err_to_name(err));
        return 1;
    }

    printf("policy %s: %u bytes\n",
           policy_storage_slot_name(slot),
           static_cast<unsigned>(length));
    if (length > 0) {
        fwrite(contents, 1, length, stdout);
        if (contents[length - 1] != '\n') {
            printf("\n");
        }
    }

    free(contents);
    return 0;
}

int policy_command(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_policy_usage();
        return argc < 2 ? 1 : 0;
    }

    if (strcmp(argv[1], "upload") == 0) {
        uint8_t expected_sha1[kChecksumSha1Bytes] = {};
        if (argc != 3 || !checksum_parse_sha1_hex(argv[2], expected_sha1)) {
            print_policy_usage();
            return 1;
        }

        return policy_upload_staged(expected_sha1);
    }

    if (strcmp(argv[1], "accept") == 0) {
        if (argc != 2) {
            print_policy_usage();
            return 1;
        }

        char *staged = nullptr;
        size_t staged_length = 0;
        esp_err_t err = policy_storage_read_alloc(PolicySlot::Staged, &staged, &staged_length);
        if (err != ESP_OK) {
            printf("policy accept failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        char lua_error[192] = {};
        err = policy_validate(staged, staged_length, lua_error, sizeof(lua_error));
        if (err != ESP_OK) {
            free(staged);
            printf("policy accept rejected: %s\n",
                   lua_error[0] != '\0' ? lua_error : esp_err_to_name(err));
            return 1;
        }

        char sha1_hex[kChecksumSha1HexChars + 1] = {};
        if (checksum_sha1_hex(staged, staged_length, sha1_hex) != ESP_OK) {
            sha1_hex[0] = '\0';
        }
        free(staged);

        err = policy_storage_accept_staged();
        if (err != ESP_OK) {
            printf("policy accept failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        printf("accepted staged policy as active\n");
        ESP_LOGI(kTag,
                 "policy accepted: %u bytes sha1=%s",
                 static_cast<unsigned>(staged_length),
                 sha1_hex);
        return 0;
    }

    print_policy_usage();
    return 1;
}

const char *task_state_name(eTaskState state)
{
    switch (state) {
    case eRunning:
        return "run";
    case eReady:
        return "ready";
    case eBlocked:
        return "block";
    case eSuspended:
        return "susp";
    case eDeleted:
        return "delete";
    case eInvalid:
        return "invalid";
    default:
        return "?";
    }
}

void print_heap_line(const char *name, uint32_t caps)
{
    const size_t total = heap_caps_get_total_size(caps);
    const size_t free_now = heap_caps_get_free_size(caps);
    const size_t min_free = heap_caps_get_minimum_free_size(caps);
    const size_t largest = heap_caps_get_largest_free_block(caps);

    if (total == 0) {
        return;
    }

    printf("%-9s total=%7u used=%7u free=%7u min_free=%7u largest=%7u\n",
           name,
           static_cast<unsigned>(total),
           static_cast<unsigned>(total - free_now),
           static_cast<unsigned>(free_now),
           static_cast<unsigned>(min_free),
           static_cast<unsigned>(largest));
}

void print_nvs_stats(void)
{
    nvs_stats_t stats = {};
    size_t partition_size = 0;
    const esp_err_t size_err = policy_storage_get_partition_size(&partition_size);
    const esp_err_t err = policy_storage_get_stats(&stats);
    if (err != ESP_OK) {
        printf("\nnvs: unavailable (%s)\n", esp_err_to_name(err));
        return;
    }

    printf("\nnvs:\n");
    if (size_err == ESP_OK) {
        printf("partition_size=%u bytes\n", static_cast<unsigned>(partition_size));
    } else {
        printf("partition_size=unknown (%s)\n", esp_err_to_name(size_err));
    }
    printf("entries used=%u free=%u total=%u namespace=%u\n",
           static_cast<unsigned>(stats.used_entries),
           static_cast<unsigned>(stats.free_entries),
           static_cast<unsigned>(stats.total_entries),
           static_cast<unsigned>(stats.namespace_count));
}

void print_task_list(void)
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks =
        static_cast<TaskStatus_t *>(calloc(task_count, sizeof(TaskStatus_t)));
    if (tasks == nullptr) {
        printf("tasks: unable to allocate task snapshot for %u tasks\n",
               static_cast<unsigned>(task_count));
        return;
    }

    uint32_t total_runtime = 0;
    task_count = uxTaskGetSystemState(tasks, task_count, &total_runtime);

    printf("\ntasks: %u\n", static_cast<unsigned>(task_count));
    printf("%-18s %-7s %4s ", "name", "state", "prio");
#if configTASKLIST_INCLUDE_COREID
    printf("%5s ", "core");
#endif
    printf("%10s %4s\n",
           "stack_hw",
           "num");

    for (UBaseType_t i = 0; i < task_count; ++i) {
        const TaskStatus_t &task = tasks[i];

        printf("%-18s %-7s %4u ",
               task.pcTaskName,
               task_state_name(task.eCurrentState),
               static_cast<unsigned>(task.uxCurrentPriority));

#if configTASKLIST_INCLUDE_COREID
        const char *core = "any";
        char core_buf[8] = {};

        if (task.xCoreID != tskNO_AFFINITY) {
            snprintf(core_buf, sizeof(core_buf), "%d", static_cast<int>(task.xCoreID));
            core = core_buf;
        }

        printf("%5s ", core);
#endif

        printf("%10u %4u\n",
               static_cast<unsigned>(task.usStackHighWaterMark),
               static_cast<unsigned>(task.xTaskNumber));
    }

    if (total_runtime == 0) {
        printf("\nruntime stats: disabled\n");
    }

    free(tasks);
#else
    printf("\ntasks: unavailable; enable CONFIG_FREERTOS_USE_TRACE_FACILITY\n");
#endif
}

int system_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip = {};
    uint32_t flash_size = 0;
    esp_chip_info(&chip);
    const esp_err_t flash_err = esp_flash_get_size(nullptr, &flash_size);
    const uint64_t uptime_s = static_cast<uint64_t>(esp_timer_get_time() / 1000000LL);
    const uint64_t uptime_days = uptime_s / 86400ULL;
    const uint64_t uptime_hours = (uptime_s / 3600ULL) % 24ULL;
    const uint64_t uptime_minutes = (uptime_s / 60ULL) % 60ULL;
    const uint64_t uptime_seconds = uptime_s % 60ULL;

    printf("app: %s version=%s idf=%s built=%s %s\n",
           app->project_name,
           app->version,
           IDF_VER,
           app->date,
           app->time);
    const BoardConfig *board = board_config_get();
    if (board != nullptr) {
        printf("board: profile=%s model=\"%s\"\n", board->profile, board->model);
    } else {
        printf("board: unavailable (%s)\n", board_config_error());
    }
    printf("uptime: %" PRIu64 "s (%" PRIu64 "d %" PRIu64 "h %" PRIu64 "m %" PRIu64 "s)\n",
           uptime_s,
           uptime_days,
           uptime_hours,
           uptime_minutes,
           uptime_seconds);
    printf("chip: model=%s revision=%u cores=%u features=0x%08" PRIx32 "\n",
           CONFIG_IDF_TARGET,
           chip.revision,
           chip.cores,
           chip.features);
    if (flash_err == ESP_OK) {
        printf("flash: %" PRIu32 " bytes\n", flash_size);
    } else {
        printf("flash: unknown (%s)\n", esp_err_to_name(flash_err));
    }
    printf("reset reason: %d\n", static_cast<int>(esp_reset_reason()));

    printf("\nheap bytes:\n");
    print_heap_line("all", MALLOC_CAP_8BIT);
    print_heap_line("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if CONFIG_SPIRAM
    print_heap_line("spiram", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif

    print_nvs_stats();
    print_task_list();
    return 0;
}

int power4_help_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("power4 commands:\n");
    printf("\n");
    printf("show:\n");
    printf("  show batteries              list observed batteries\n");
    printf("  show banks                  list configured battery banks\n");
    printf("  show ble                    list BLE GAP procedure and connection state\n");
    printf("  show board                  show the selected hardware profile\n");
    printf("  show debug                  show volatile debug settings\n");
    printf("  show ethernet               show Ethernet settings and link state\n");
    printf("  show espnow                 show ESP-NOW configuration and counters\n");
    printf("  show inputs                 list digital input state\n");
    printf("  show logs                   print recent system log text\n");
    printf("  show password               reveal TCP password (serial only)\n");
    printf("  show policy                 print the active policy program\n");
    printf("  show policy staged          print the staged policy program\n");
    printf("  show policy parameters      list persistent policy parameters\n");
    printf("  show relays                 list relay state\n");
    printf("  show system                 show firmware, uptime, heap, NVS, and tasks\n");
    printf("  show time                   show UTC and timezone-adjusted system time\n");
    printf("  show timezone               show POSIX timezone and SNTP state\n");
    printf("  show timezones              list supported timezone names and rules\n");
    printf("\n");
    printf("report:\n");
    printf("  report banks                print framed JSON battery-bank state\n");
    printf("  report batteries            print framed JSON battery observations\n");
    printf("  report inputs               print framed JSON digital input state\n");
    printf("  report logs                 print framed JSON recent system log text\n");
    printf("  report parameters           print framed JSON policy parameters\n");
    printf("  report relays               print framed JSON relay state\n");
    printf("\n");
    printf("set:\n");
    printf("  set debug ble_scanner on    enable verbose BLE scanner logging\n");
    printf("  set debug ble_scanner off   disable verbose BLE scanner logging\n");
    printf("  set ethernet dhcp           use DHCP and save the setting\n");
    printf("  set ethernet static <ip> <netmask> <gateway> [dns1] [dns2]\n");
    printf("  set ethernet phy <mode>     auto, 10-half, 10-full, 100-half, or 100-full\n");
    printf("  set espnow name <name>      set the report sender name\n");
    printf("  set espnow peer <name> <mac|none>\n");
    printf("  set espnow channel <1-14|off>\n");
    printf("  set espnow rate <auto|lr-500|lr-250>\n");
    printf("  set espnow gateway <ipv4> <port>|none\n");
    printf("  set password [password]     generate or set TCP password (serial only)\n");
    printf("  set relay <n> on [seconds]  turn relay on for a bounded time\n");
    printf("  set relay <n> force-on      force relay on administratively\n");
    printf("  set relay <n> force-off     force relay off, overriding any timer\n");
    printf("  set relay <n> clear-force   clear administrative force\n");
    printf("  set time <date> <time>      set RTC and system UTC time\n");
    printf("  set timezone <name>         select a name from 'show timezones'\n");
    printf("\n");
    printf("define/remove:\n");
    printf("  define bank <name> <battery> [battery...]\n");
    printf("  remove bank <name>\n");
    printf("  define policy <name>=true|false|<number> [<seconds>s]\n");
    printf("  remove policy <name>\n");
    printf("\n");
    printf("policy program:\n");
    printf("  policy upload <sha1-hex>    upload base64 staged policy text\n");
    printf("  policy accept               accept staged policy as active\n");
    printf("\n");
    printf("system:\n");
    printf("  reboot                      restart the controller\n");
    return 0;
}

int reboot_command(int argc, char **argv)
{
    if (argc != 1) {
        printf("usage:\n");
        printf("  reboot\n");
        return 1;
    }

    printf("rebooting\n");
    fflush(stdout);
    if (g_reboot_timer == nullptr) {
        const esp_timer_create_args_t timer_args = {
            .callback = &reboot_timer_callback,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "reboot",
            .skip_unhandled_events = true,
        };
        const esp_err_t create_err = esp_timer_create(&timer_args, &g_reboot_timer);
        if (create_err != ESP_OK) {
            printf("reboot failed: %s\n", esp_err_to_name(create_err));
            return 1;
        }
    }
    const esp_err_t start_err = esp_timer_start_once(g_reboot_timer, 250000);
    if (start_err != ESP_OK) {
        printf("reboot failed: %s\n", esp_err_to_name(start_err));
        return 1;
    }
    return 0;
}

void register_console_command(const char *command,
                              const char *help,
                              esp_console_cmd_func_t func)
{
    esp_console_cmd_t definition = {};
    definition.command = command;
    definition.help = help;
    definition.func = func;

    ESP_ERROR_CHECK(esp_console_cmd_register(&definition));
}

void register_commands(void)
{
    register_console_command("help", "Show command summary", &power4_help_command);
    register_console_command("show", "Show observed controller state", &show_command);
    register_console_command("set", "Set volatile controller state", &set_command);
    register_console_command("define", "Define persistent controller state", &define_command);
    register_console_command("remove", "Remove persistent controller state", &remove_command);
    register_console_command("report", "Print framed JSON state reports", &report_command);
    register_console_command("policy", "Upload and accept policy programs", &policy_command);
    register_console_command("reboot", "Restart the controller", &reboot_command);
}

esp_err_t setup_console_device(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    usb_serial_jtag_driver_config_t usb_serial_jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_config.rx_buffer_size = kConsoleRxBufferBytes;
    esp_err_t err = usb_serial_jtag_driver_install(&usb_serial_jtag_config);
    if (err != ESP_OK) {
        return err;
    }
    usb_serial_jtag_vfs_use_driver();
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_vfs_dev_cdcacm_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    esp_vfs_dev_cdcacm_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t dev_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    uart_vfs_dev_port_set_rx_line_endings(dev_config.channel, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(dev_config.channel, ESP_LINE_ENDINGS_CRLF);

    const uart_config_t uart_config = {
        .baud_rate = dev_config.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(dev_config.channel, &uart_config);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(dev_config.channel,
                       dev_config.tx_gpio_num,
                       dev_config.rx_gpio_num,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_driver_install(dev_config.channel, 256, 0, 0, nullptr, 0);
    if (err != ESP_OK) {
        return err;
    }
    uart_vfs_dev_use_driver(dev_config.channel);
#else
#error "No ESP-IDF console device is configured"
#endif

    fcntl(fileno(stdout), F_SETFL, 0);
    fcntl(fileno(stdin), F_SETFL, 0);
    setvbuf(stdin, nullptr, _IONBF, 0);
    setvbuf(stdout, nullptr, _IONBF, 0);

    esp_console_config_t console_config = ESP_CONSOLE_CONFIG_DEFAULT();
    console_config.max_cmdline_length = kConsoleLineBytes;
    return esp_console_init(&console_config);
}

void run_console_line(char *line)
{
    if (line == nullptr || line[0] == '\0') {
        return;
    }

    int command_result = 0;
    const esp_err_t err =
        power4_console_execute(line,
                               Power4CommandSource::Serial,
                               nullptr,
                               nullptr,
                               &command_result);
    if (err == ESP_ERR_TIMEOUT) {
        printf("console busy; try again\n");
    }
}

void console_task(void *arg)
{
    (void)arg;

    char line[kConsoleLineBytes] = {};
    size_t length = 0;
    EscapeState escape_state = EscapeState::None;

    update_console_line_snapshot(line, length, true);
    draw_prompt_line(line, length);

    while (true) {
        char ch = '\0';
        const int read_count = fgetc(stdin);
        if (read_count == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        ch = static_cast<char>(read_count);

        if (consume_escape_sequence(ch, &escape_state)) {
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            printf("\r\n");
            line[length] = '\0';
            update_console_line_snapshot(nullptr, 0, false);
            run_console_line(line);
            length = 0;
            line[0] = '\0';
            escape_state = EscapeState::None;
            update_console_line_snapshot(line, length, true);
            draw_prompt_line(line, length);
            continue;
        }

        if (ch == '\x12') {
            redraw_line(line, length);
            continue;
        }

        if (ch == '\x15') {
            clear_input_line(line, &length);
            update_console_line_snapshot(line, length, true);
            continue;
        }

        if (ch == '\b' || ch == '\x7f') {
            if (length > 0) {
                --length;
                line[length] = '\0';
                update_console_line_snapshot(line, length, true);
                erase_one_display_char();
            }
            continue;
        }

        if (!isprint(static_cast<unsigned char>(ch))) {
            continue;
        }

        if (length + 1 >= sizeof(line)) {
            putchar('\a');
            fflush(stdout);
            continue;
        }

        line[length] = ch;
        ++length;
        line[length] = '\0';
        update_console_line_snapshot(line, length, true);
        putchar(ch);
        fflush(stdout);
    }
}

}  // namespace

esp_err_t power4_console_execute(const char *command,
                                 Power4CommandSource source,
                                 FILE *input,
                                 FILE *output,
                                 int *command_result)
{
    if (command == nullptr || command[0] == '\0' || command_result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_command_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool framed =
        strncmp(command, kToolCommandPrefix, sizeof(kToolCommandPrefix) - 1) == 0;
    const char *actual_command =
        framed ? command + sizeof(kToolCommandPrefix) - 1 : command;
    if (framed && actual_command[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    // A failed network client must not be able to make the serial recovery
    // console wait forever. Socket I/O has its own deadlines, and this bound
    // also keeps a second command from becoming permanently stuck behind it.
    if (xSemaphoreTake(g_command_mutex, kCommandMutexTimeoutTicks) != pdTRUE) {
        if (framed) {
            write_console_busy_frame(output != nullptr ? output : stdout);
        }
        return ESP_ERR_TIMEOUT;
    }

    FILE *previous_stdin = stdin;
    FILE *previous_stdout = stdout;
    FILE *destination = output != nullptr ? output : previous_stdout;
    DotStuffStream dot_context = {
        .destination = destination,
        .line_start = true,
    };
    FILE *dot_stream = nullptr;
    if (framed) {
        dot_stream = funopen(&dot_context, nullptr, dot_stuff_write, nullptr, nullptr);
        if (dot_stream == nullptr) {
            xSemaphoreGive(g_command_mutex);
            write_console_busy_frame(destination);
            return ESP_ERR_NO_MEM;
        }
        setvbuf(dot_stream, nullptr, _IONBF, 0);
    }
    if (input != nullptr) {
        stdin = input;
    }
    if (framed) {
        stdout = dot_stream;
        g_capture_task = xTaskGetCurrentTaskHandle();
    } else if (output != nullptr) {
        stdout = destination;
        g_capture_task = xTaskGetCurrentTaskHandle();
    }
    g_command_source = source;

    *command_result = 0;
    const esp_err_t err = esp_console_run(actual_command, command_result);
    if (err == ESP_ERR_NOT_FOUND) {
        printf("unknown command: %s\n", actual_command);
        *command_result = 1;
    } else if (err == ESP_ERR_INVALID_ARG) {
        printf("invalid command line: %s\n", actual_command);
        *command_result = 1;
    } else if (err != ESP_OK) {
        printf("command failed: %s\n", esp_err_to_name(err));
        *command_result = 1;
    }
    fflush(stdout);
    if (framed) {
        finish_dot_stuffed_response(&dot_context);
    }

    g_command_source = Power4CommandSource::Serial;
    g_capture_task = nullptr;
    stdin = previous_stdin;
    stdout = previous_stdout;
    if (dot_stream != nullptr) {
        fclose(dot_stream);
    }
    xSemaphoreGive(g_command_mutex);
    return err;
}

Power4CommandSource power4_console_command_source(void)
{
    return g_command_source;
}

esp_err_t power4_console_start(void)
{
    esp_err_t err = setup_console_device();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "failed to set up console: %s", esp_err_to_name(err));
        return err;
    }

    g_previous_log_vprintf = esp_log_set_vprintf(console_log_vprintf);
    register_commands();
    g_command_mutex = xSemaphoreCreateMutexStatic(&g_command_mutex_storage);
    if (g_command_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreate(console_task,
                                           "console",
                                           kConsoleTaskStackBytes,
                                           nullptr,
                                           kConsoleTaskPriority,
                                           nullptr);
    if (created != pdPASS) {
        ESP_LOGE(kTag, "failed to start console task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "console started; type 'help' for commands");
    return ESP_OK;
}
