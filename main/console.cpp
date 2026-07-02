#include "console.hpp"

#include <inttypes.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "checksum.hpp"
#include "config_flags.hpp"
#include "json_output.hpp"
#include "policy_storage.hpp"
#include "relay_manager.hpp"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

namespace {

constexpr const char *kTag = "power4_console";
constexpr const char *kPrompt = "power4> ";
constexpr size_t kRelayStateJsonBaseBytes = 96;
constexpr size_t kRelayStateJsonBytesPerRelay = 160;
constexpr size_t kPolicyUploadMaxDecodedBytes = 8192;
constexpr size_t kPolicyUploadMaxEncodedBytes = ((kPolicyUploadMaxDecodedBytes + 2) / 3) * 4;
constexpr size_t kPolicyUploadLineBytes = 160;

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

bool parse_relay(const char *text, uint8_t *relay)
{
    uint32_t parsed = 0;
    if (!parse_u32(text, &parsed) || parsed == 0 || parsed > relay_manager_count()) {
        return false;
    }

    *relay = static_cast<uint8_t>(parsed);
    return true;
}

void print_relay_usage(void)
{
    printf("usage:\n");
    printf("  relay list\n");
    printf("  relay query <relay>\n");
    printf("  relay state\n");
    printf("  relay on <relay> <seconds>   (seconds must be > 0)\n");
    printf("  relay force-on <relay>\n");
    printf("  relay clear-force <relay>\n");
    printf("relays are numbered 1-%u\n", relay_manager_count());
}

void print_relay_status(const RelayStatus &status)
{
    printf("relay %u: gpio=%d active_level=%u output=%s timer=%s",
           status.relay,
           status.gpio_pin,
           status.active_level,
           status.output_on ? "on" : "off",
           status.timer_active ? "on" : "off");
    if (status.timer_active) {
        printf(" remaining=%" PRIu32 "s", status.timer_remaining_s);
    }
    printf(" override=%s\n", status.forced_on ? "on" : "off");
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
    int result = 0;
    for (uint8_t relay = 1; relay <= relay_manager_count(); ++relay) {
        result |= query_and_print_relay(relay);
    }
    return result;
}

int print_compact_relay_states(void)
{
    printf("relays:");

    for (uint8_t relay = 1; relay <= relay_manager_count(); ++relay) {
        RelayStatus status = {};
        const esp_err_t err = relay_manager_query(relay, &status);
        if (err != ESP_OK) {
            printf(" %u=error:%s", relay, esp_err_to_name(err));
            continue;
        }

        printf(" %u=%s", relay, status.output_on ? "on" : "off");
    }

    printf("\n");
    return 0;
}

int print_config_flags(void)
{
    ConfigFlagList flags = {};
    const esp_err_t err = config_flags_list(&flags);
    if (err != ESP_OK) {
        printf("set: error:%s\n", esp_err_to_name(err));
        return 1;
    }

    printf("set:");
    if (flags.count == 0) {
        printf(" none");
    }
    for (size_t i = 0; i < flags.count; ++i) {
        printf(" %s", flags.names[i]);
    }
    if (flags.truncated) {
        printf(" ...");
    }
    printf("\n");
    return flags.truncated ? 1 : 0;
}

int status_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("power4 console is running\n");
    printf("mode: startup\n");
    int result = print_compact_relay_states();
    result |= print_config_flags();
    return result;
}

void print_set_usage(const char *command)
{
    printf("usage: %s <name>\n", command);
    printf("name: 1-15 characters, letters/digits/_/-\n");
}

int set_command(int argc, char **argv)
{
    if (argc != 2 || !config_flags_valid_name(argv[1])) {
        print_set_usage("set");
        return 1;
    }

    const esp_err_t err = config_flags_set(argv[1]);
    if (err != ESP_OK) {
        printf("set %s failed: %s\n", argv[1], esp_err_to_name(err));
        return 1;
    }

    printf("set %s\n", argv[1]);
    return 0;
}

int unset_command(int argc, char **argv)
{
    if (argc != 2 || !config_flags_valid_name(argv[1])) {
        print_set_usage("unset");
        return 1;
    }

    const esp_err_t err = config_flags_unset(argv[1]);
    if (err != ESP_OK) {
        printf("unset %s failed: %s\n", argv[1], esp_err_to_name(err));
        return 1;
    }

    printf("unset %s\n", argv[1]);
    return 0;
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

int relay_state_command(void)
{
    const uint8_t relay_count = relay_manager_count();
    const size_t capacity =
        kRelayStateJsonBaseBytes + (static_cast<size_t>(relay_count) * kRelayStateJsonBytesPerRelay);
    char *json = static_cast<char *>(malloc(capacity));
    if (json == nullptr) {
        printf("relay state failed: out of memory\n");
        return 1;
    }

    size_t used = 0;
    bool ok = append_json(json,
                          capacity,
                          &used,
                          "{\"type\":\"relay_state\",\"relay_count\":%u,\"relays\":[",
                          relay_count);

    for (uint8_t relay = 1; ok && relay <= relay_count; ++relay) {
        RelayStatus status = {};
        const esp_err_t err = relay_manager_query(relay, &status);
        if (err != ESP_OK) {
            printf("relay state failed: relay %u query: %s\n", relay, esp_err_to_name(err));
            free(json);
            return 1;
        }

        ok = append_json(json,
                         capacity,
                         &used,
                         "%s{\"id\":%u,\"gpio\":%d,\"active_level\":%u,"
                         "\"output_on\":%s,\"timer_active\":%s,"
                         "\"timer_remaining_s\":%" PRIu32 ",\"admin_override\":%s}",
                         relay == 1 ? "" : ",",
                         status.relay,
                         status.gpio_pin,
                         status.active_level,
                         status.output_on ? "true" : "false",
                         status.timer_active ? "true" : "false",
                         status.timer_remaining_s,
                         status.forced_on ? "true" : "false");
    }

    ok = ok && append_json(json, capacity, &used, "]}");
    if (!ok) {
        printf("relay state failed: JSON buffer too small\n");
        free(json);
        return 1;
    }

    const esp_err_t err = json_output_print(json);
    free(json);
    if (err != ESP_OK) {
        printf("relay state failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    return 0;
}

int relay_command(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_relay_usage();
        return argc < 2 ? 1 : 0;
    }

    if (strcmp(argv[1], "list") == 0) {
        if (argc != 2) {
            print_relay_usage();
            return 1;
        }
        return print_all_relays();
    }

    if (strcmp(argv[1], "query") == 0) {
        uint8_t relay = 0;
        if (argc != 3 || !parse_relay(argv[2], &relay)) {
            print_relay_usage();
            return 1;
        }
        return query_and_print_relay(relay);
    }

    if (strcmp(argv[1], "state") == 0) {
        if (argc != 2) {
            print_relay_usage();
            return 1;
        }
        return relay_state_command();
    }

    if (strcmp(argv[1], "on") == 0) {
        uint8_t relay = 0;
        uint32_t seconds = 0;
        if (argc != 4 || !parse_relay(argv[2], &relay) || !parse_u32(argv[3], &seconds) ||
            seconds == 0) {
            print_relay_usage();
            return 1;
        }

        const esp_err_t err = relay_manager_on_for(relay, seconds);
        if (err != ESP_OK) {
            printf("relay on failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        return query_and_print_relay(relay);
    }

    if (strcmp(argv[1], "force-on") == 0) {
        uint8_t relay = 0;
        if (argc != 3 || !parse_relay(argv[2], &relay)) {
            print_relay_usage();
            return 1;
        }

        const esp_err_t err = relay_manager_force_on(relay);
        if (err != ESP_OK) {
            printf("relay force-on failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        return query_and_print_relay(relay);
    }

    if (strcmp(argv[1], "clear-force") == 0) {
        uint8_t relay = 0;
        if (argc != 3 || !parse_relay(argv[2], &relay)) {
            print_relay_usage();
            return 1;
        }

        const esp_err_t err = relay_manager_clear_force(relay);
        if (err != ESP_OK) {
            printf("relay clear-force failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        return query_and_print_relay(relay);
    }

    print_relay_usage();
    return 1;
}

void print_config_usage(void)
{
    printf("usage:\n");
    printf("  config show [active]\n");
    printf("  config show staged\n");
    printf("  config upload staged <sha1-hex>\n");
    printf("  config accept staged\n");
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

int config_upload_staged(const uint8_t expected_sha1[kChecksumSha1Bytes])
{
    char *encoded = static_cast<char *>(malloc(kPolicyUploadMaxEncodedBytes + 1));
    uint8_t *decoded = static_cast<uint8_t *>(malloc(kPolicyUploadMaxDecodedBytes + 1));
    if (encoded == nullptr || decoded == nullptr) {
        printf("config upload staged failed: out of memory\n");
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
            printf("config upload staged failed: encoded input too large\n");
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
        printf("config upload staged failed: invalid base64 (%d)\n", decode_result);
        free(encoded);
        free(decoded);
        return 1;
    }

    decoded[decoded_length] = '\0';
    uint8_t actual_sha1[kChecksumSha1Bytes] = {};
    esp_err_t err = checksum_sha1(decoded, decoded_length, actual_sha1);
    if (err != ESP_OK) {
        printf("config upload staged failed: checksum calculation: %s\n", esp_err_to_name(err));
        free(encoded);
        free(decoded);
        return 1;
    }

    char expected_hex[kChecksumSha1HexChars + 1] = {};
    char actual_hex[kChecksumSha1HexChars + 1] = {};
    checksum_bytes_to_hex(expected_sha1, kChecksumSha1Bytes, expected_hex, sizeof(expected_hex));
    checksum_bytes_to_hex(actual_sha1, kChecksumSha1Bytes, actual_hex, sizeof(actual_hex));

    if (memcmp(actual_sha1, expected_sha1, kChecksumSha1Bytes) != 0) {
        printf("config upload staged failed: checksum mismatch expected=%s actual=%s\n",
               expected_hex,
               actual_hex);
        free(encoded);
        free(decoded);
        return 1;
    }

    err = policy_storage_write_staged(decoded, decoded_length);
    if (err != ESP_OK) {
        printf("config upload staged failed: %s\n", esp_err_to_name(err));
        free(encoded);
        free(decoded);
        return 1;
    }

    printf("uploaded staged configuration: %u bytes sha1=%s\n",
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
        printf("config show %s failed: %s\n",
               policy_storage_slot_name(slot),
               esp_err_to_name(err));
        return 1;
    }

    printf("config %s: %u bytes\n",
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

int config_command(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_config_usage();
        return argc < 2 ? 1 : 0;
    }

    if (strcmp(argv[1], "show") == 0) {
        if (argc == 2 || (argc == 3 && strcmp(argv[2], "active") == 0)) {
            return print_policy_slot(PolicySlot::Active);
        }
        if (argc == 3 && strcmp(argv[2], "staged") == 0) {
            return print_policy_slot(PolicySlot::Staged);
        }

        print_config_usage();
        return 1;
    }

    if (strcmp(argv[1], "upload") == 0) {
        uint8_t expected_sha1[kChecksumSha1Bytes] = {};
        if (argc != 4 || strcmp(argv[2], "staged") != 0 ||
            !checksum_parse_sha1_hex(argv[3], expected_sha1)) {
            print_config_usage();
            return 1;
        }

        return config_upload_staged(expected_sha1);
    }

    if (strcmp(argv[1], "accept") == 0) {
        if (argc != 3 || strcmp(argv[2], "staged") != 0) {
            print_config_usage();
            return 1;
        }

        const esp_err_t err = policy_storage_accept_staged();
        if (err != ESP_OK) {
            printf("config accept staged failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        printf("accepted staged configuration as active\n");
        return 0;
    }

    print_config_usage();
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

    printf("app: %s version=%s idf=%s built=%s %s\n",
           app->project_name,
           app->version,
           IDF_VER,
           app->date,
           app->time);
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

void register_commands(void)
{
    esp_console_register_help_command();

    esp_console_cmd_t status = {};
    status.command = "status";
    status.help = "Print a short controller status summary";
    status.func = &status_command;

    ESP_ERROR_CHECK(esp_console_cmd_register(&status));

    esp_console_cmd_t set = {};
    set.command = "set";
    set.help = "Set a named boolean config flag";
    set.func = &set_command;

    ESP_ERROR_CHECK(esp_console_cmd_register(&set));

    esp_console_cmd_t unset = {};
    unset.command = "unset";
    unset.help = "Clear a named boolean config flag";
    unset.func = &unset_command;

    ESP_ERROR_CHECK(esp_console_cmd_register(&unset));

    esp_console_cmd_t system = {};
    system.command = "system";
    system.help = "Print system, heap, and task diagnostics";
    system.func = &system_command;

    ESP_ERROR_CHECK(esp_console_cmd_register(&system));

    esp_console_cmd_t relay = {};
    relay.command = "relay";
    relay.help = "Control and inspect relay timers and overrides";
    relay.func = &relay_command;

    ESP_ERROR_CHECK(esp_console_cmd_register(&relay));

    esp_console_cmd_t config = {};
    config.command = "config";
    config.help = "Inspect and accept policy configuration";
    config.func = &config_command;

    ESP_ERROR_CHECK(esp_console_cmd_register(&config));
}

esp_err_t create_repl(esp_console_repl_t **repl)
{
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = kPrompt;
    repl_config.max_cmdline_length = 256;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    return esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, repl);
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t dev_config =
        ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    return esp_console_new_repl_usb_cdc(&dev_config, &repl_config, repl);
#elif CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t dev_config =
        ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    return esp_console_new_repl_uart(&dev_config, &repl_config, repl);
#else
#error "No ESP-IDF console device is configured"
#endif
}

}  // namespace

esp_err_t power4_console_start(void)
{
    register_commands();

    esp_console_repl_t *repl = nullptr;
    esp_err_t err = create_repl(&repl);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "failed to create console REPL: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "failed to start console REPL: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(kTag, "console started; type 'help' for commands");
    return ESP_OK;
}
