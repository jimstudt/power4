#include "state_report.hpp"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "battery_bank.hpp"
#include "battery_store.hpp"
#include "board_config.hpp"
#include "esp_timer.h"
#include "input_manager.hpp"
#include "jbd_protocol.hpp"
#include "relay_manager.hpp"
#include "sdkconfig.h"

namespace {

constexpr size_t kRelayBaseBytes = 96;
constexpr size_t kRelayBytesPerRelay = 224;
constexpr size_t kInputBaseBytes = 96;
constexpr size_t kInputBytesPerInput = 192;
constexpr size_t kBatteryBaseBytes = 96;
constexpr size_t kBatteryBytesPerBattery = 1024;
constexpr size_t kBankBaseBytes = 96;
constexpr size_t kBankBytesPerBank = 896;

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

bool append_json_string(char *buffer, size_t capacity, size_t *used, const char *value)
{
    if (!append_json(buffer, capacity, used, "\"")) {
        return false;
    }
    for (const char *cursor = value; cursor != nullptr && *cursor != '\0'; ++cursor) {
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

esp_err_t finish_report(char *buffer,
                        size_t capacity,
                        size_t used,
                        bool ok,
                        char **json,
                        size_t *length)
{
    if (!ok || used >= capacity) {
        free(buffer);
        return ESP_ERR_INVALID_SIZE;
    }
    *json = buffer;
    *length = used;
    return ESP_OK;
}

esp_err_t build_relays(char **json, size_t *length)
{
    const uint8_t count = relay_manager_count();
    const size_t capacity = kRelayBaseBytes + (static_cast<size_t>(count) * kRelayBytesPerRelay);
    char *buffer = static_cast<char *>(malloc(capacity));
    if (buffer == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    size_t used = 0;
    bool ok = append_json(buffer,
                          capacity,
                          &used,
                          "{\"type\":\"relay_state\",\"relay_count\":%u,\"relays\":[",
                          count);
    for (uint8_t relay = 1; ok && relay <= count; ++relay) {
        RelayStatus status = {};
        const esp_err_t err = relay_manager_query(relay, &status);
        if (err != ESP_OK) {
            free(buffer);
            return err;
        }
        ok = append_json(buffer,
                         capacity,
                         &used,
                         "%s{\"id\":%u,\"backend\":\"%s\",\"hardware_channel\":%d,"
                         "\"gpio\":%d,\"active_level\":%u,\"output_on\":%s,"
                         "\"timer_active\":%s,\"timer_remaining_s\":%" PRIu32
                         ",\"force\":\"%s\"}",
                         relay == 1 ? "" : ",",
                         status.relay,
                         relay_backend_name(status.backend),
                         status.hardware_channel,
                         status.gpio_pin,
                         status.active_level,
                         status.output_on ? "true" : "false",
                         status.timer_active ? "true" : "false",
                         status.timer_remaining_s,
                         relay_force_name(status.force));
    }
    ok = ok && append_json(buffer, capacity, &used, "]}");
    return finish_report(buffer, capacity, used, ok, json, length);
}

esp_err_t build_inputs(char **json, size_t *length)
{
    InputManagerStatus manager = {};
    esp_err_t err = input_manager_get_status(&manager);
    if (err != ESP_OK) {
        return err;
    }
    if (manager.present && !manager.initialized) {
        return manager.initialization_result;
    }

    const uint8_t count = manager.initialized ? manager.count : 0;
    const size_t capacity = kInputBaseBytes + (static_cast<size_t>(count) * kInputBytesPerInput);
    char *buffer = static_cast<char *>(malloc(capacity));
    if (buffer == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    size_t used = 0;
    bool ok = append_json(buffer,
                          capacity,
                          &used,
                          "{\"type\":\"input_state\",\"input_count\":%u,\"inputs\":[",
                          count);
    for (uint8_t input = 1; ok && input <= count; ++input) {
        InputStatus status = {};
        err = input_manager_query(input, &status);
        if (err != ESP_OK) {
            free(buffer);
            return err;
        }
        ok = append_json(buffer,
                         capacity,
                         &used,
                         "%s{\"id\":%u,\"backend\":\"%s\",\"hardware_channel\":%d,"
                         "\"gpio\":%d,\"active_level\":%u,\"level\":%d,\"input_on\":%s}",
                         input == 1 ? "" : ",",
                         status.input,
                         digital_input_backend_name(status.backend),
                         status.gpio_pin,
                         status.gpio_pin,
                         status.active_level,
                         status.level,
                         status.on ? "true" : "false");
    }
    ok = ok && append_json(buffer, capacity, &used, "]}");
    return finish_report(buffer, capacity, used, ok, json, length);
}

esp_err_t build_batteries(char **json, size_t *length)
{
    BatteryRecord *records =
        static_cast<BatteryRecord *>(calloc(CONFIG_POWER4_MAX_BATTERIES, sizeof(BatteryRecord)));
    if (records == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0;
    esp_err_t err = battery_store_snapshot(records, CONFIG_POWER4_MAX_BATTERIES, &count);
    if (err != ESP_OK) {
        free(records);
        return err;
    }

    const size_t capacity =
        kBatteryBaseBytes + (CONFIG_POWER4_MAX_BATTERIES * kBatteryBytesPerBattery);
    char *buffer = static_cast<char *>(malloc(capacity));
    if (buffer == nullptr) {
        free(records);
        return ESP_ERR_NO_MEM;
    }

    size_t used = 0;
    const int64_t now_us = esp_timer_get_time();
    bool ok = append_json(buffer,
                          capacity,
                          &used,
                          "{\"type\":\"battery_state\",\"capacity\":%u,\"count\":%u,"
                          "\"batteries\":[",
                          static_cast<unsigned>(battery_store_capacity()),
                          static_cast<unsigned>(count));
    for (size_t i = 0; ok && i < count; ++i) {
        ok = append_json(buffer, capacity, &used, "%s{\"name\":", i == 0 ? "" : ",");
        ok = ok && append_json_string(buffer, capacity, &used, records[i].name);
        ok = ok && append_json(buffer,
                               capacity,
                               &used,
                               ",\"voltage_v\":%.3f,\"current_a\":%.3f,"
                               "\"soc_percent\":%.1f,\"cycle_count\":%u,"
                               "\"protection_status\":%u,"
                               "\"cell_undervoltage_protection\":%s,"
                               "\"reported_cell_count\":%u,"
                               "\"temperature_c\":",
                               static_cast<double>(records[i].voltage_v),
                               static_cast<double>(records[i].current_a),
                               static_cast<double>(records[i].soc_percent),
                               records[i].cycle_count,
                               records[i].protection_status,
                               (records[i].protection_status & kJbdProtectionCellUndervoltage) != 0
                                   ? "true"
                                   : "false",
                               records[i].reported_cell_count);
        if (ok && records[i].temperature_valid) {
            ok = append_json(buffer,
                             capacity,
                             &used,
                             "%.1f",
                             static_cast<double>(records[i].temperature_c));
        } else if (ok) {
            ok = append_json(buffer, capacity, &used, "null");
        }
        BatteryCellSummary cells = {};
        const bool cells_valid = battery_record_cell_summary(&records[i], &cells);
        ok = ok && append_json(buffer, capacity, &used, ",\"cell_voltages_v\":");
        if (ok && cells_valid) {
            ok = append_json(buffer, capacity, &used, "[");
            for (uint8_t cell = 0; ok && cell < records[i].cell_voltage_count; ++cell) {
                ok = append_json(buffer,
                                 capacity,
                                 &used,
                                 "%s%.3f",
                                 cell == 0 ? "" : ",",
                                 records[i].cell_voltages_mv[cell] / 1000.0);
            }
            ok = ok && append_json(buffer,
                                   capacity,
                                   &used,
                                   "],\"min_cell_voltage_v\":%.3f,"
                                   "\"max_cell_voltage_v\":%.3f,"
                                   "\"cell_delta_voltage_v\":%.3f,"
                                   "\"min_cell_number\":%u,\"max_cell_number\":%u,"
                                   "\"cell_last_seen_us\":%" PRId64 ",\"cell_age_s\":%" PRIu32,
                                   static_cast<double>(cells.min_voltage_v),
                                   static_cast<double>(cells.max_voltage_v),
                                   static_cast<double>(cells.delta_voltage_v),
                                   cells.min_cell_number,
                                   cells.max_cell_number,
                                   records[i].cell_last_seen_us,
                                   battery_record_cell_age_s(&records[i], now_us));
        } else if (ok) {
            ok = append_json(buffer,
                             capacity,
                             &used,
                             "null,\"min_cell_voltage_v\":null,"
                             "\"max_cell_voltage_v\":null,"
                             "\"cell_delta_voltage_v\":null,"
                             "\"min_cell_number\":null,\"max_cell_number\":null,"
                             "\"cell_last_seen_us\":null,\"cell_age_s\":null");
        }
        ok = ok && append_json(buffer,
                               capacity,
                               &used,
                               ",\"last_seen_us\":%" PRId64 "}",
                               records[i].last_seen_us);
    }
    free(records);
    ok = ok && append_json(buffer, capacity, &used, "]}");
    return finish_report(buffer, capacity, used, ok, json, length);
}

esp_err_t build_banks(char **json, size_t *length)
{
    BatteryBankList *banks = static_cast<BatteryBankList *>(malloc(sizeof(BatteryBankList)));
    if (banks == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = battery_bank_list(banks);
    if (err != ESP_OK) {
        free(banks);
        return err;
    }

    const size_t capacity = kBankBaseBytes + (kBatteryBankMaxBanks * kBankBytesPerBank);
    char *buffer = static_cast<char *>(malloc(capacity));
    if (buffer == nullptr) {
        free(banks);
        return ESP_ERR_NO_MEM;
    }

    size_t used = 0;
    bool ok = append_json(buffer,
                          capacity,
                          &used,
                          "{\"type\":\"battery_bank_state\",\"capacity\":%u,\"count\":%u,"
                          "\"banks\":[",
                          static_cast<unsigned>(kBatteryBankMaxBanks),
                          static_cast<unsigned>(banks->count));
    for (size_t i = 0; ok && i < banks->count; ++i) {
        const BatteryBankDefinition &bank = banks->banks[i];
        BatteryBankState state = {};
        const esp_err_t state_err = battery_bank_get_state(bank.name, &state);

        ok = append_json(buffer, capacity, &used, "%s{\"name\":", i == 0 ? "" : ",");
        ok = ok && append_json_string(buffer, capacity, &used, bank.name);
        ok = ok && append_json(buffer, capacity, &used, ",\"members\":[");
        for (size_t j = 0; ok && j < bank.battery_count; ++j) {
            ok = append_json(buffer, capacity, &used, "%s", j == 0 ? "" : ",");
            ok = ok && append_json_string(buffer, capacity, &used, bank.batteries[j]);
        }
        ok = ok && append_json(buffer,
                               capacity,
                               &used,
                               "],\"ready\":%s,\"voltage_v\":",
                               state_err == ESP_OK && state.ready ? "true" : "false");
        if (ok && state_err == ESP_OK && state.ready) {
            ok = append_json(buffer,
                             capacity,
                             &used,
                             "%.3f,\"current_a\":%.3f,\"soc_percent\":%.1f",
                             static_cast<double>(state.voltage_v),
                             static_cast<double>(state.current_a),
                             static_cast<double>(state.soc_percent));
        } else {
            ok = append_json(buffer,
                             capacity,
                             &used,
                             "null,\"current_a\":null,\"soc_percent\":null");
        }
        if (ok && state_err != ESP_OK) {
            ok = append_json(buffer, capacity, &used, ",\"error\":");
            ok = ok && append_json_string(buffer,
                                          capacity,
                                          &used,
                                          esp_err_to_name(state_err));
        }
        ok = ok && append_json(buffer, capacity, &used, ",\"protection_status\":");
        if (ok && state_err == ESP_OK && state.ready) {
            ok = append_json(buffer,
                             capacity,
                             &used,
                             "%u",
                             static_cast<unsigned>(state.protection_status));
        } else if (ok) {
            ok = append_json(buffer, capacity, &used, "null");
        }
        ok = ok && append_json(buffer, capacity, &used, ",\"cell_undervoltage_protection\":");
        if (ok && state_err == ESP_OK && state.ready) {
            ok = append_json(buffer,
                             capacity,
                             &used,
                             "%s",
                             state.cell_undervoltage_protection ? "true" : "false");
        } else if (ok) {
            ok = append_json(buffer, capacity, &used, "null");
        }
        ok = ok && append_json(buffer,
                               capacity,
                               &used,
                               ",\"cell_data_ready\":%s,\"min_cell_voltage_v\":",
                               state_err == ESP_OK && state.ready && state.cell_data_ready ? "true"
                                                                                         : "false");
        if (ok && state_err == ESP_OK && state.ready && state.cell_data_ready) {
            ok = append_json(buffer,
                             capacity,
                             &used,
                             "%.3f,\"min_cell_battery\":",
                             static_cast<double>(state.min_cell_voltage_v));
            ok = ok && append_json_string(buffer, capacity, &used, state.min_cell_battery);
            ok = ok && append_json(buffer,
                                   capacity,
                                   &used,
                                   ",\"min_cell_number\":%u,\"cell_age_s\":%" PRIu32,
                                   state.min_cell_number,
                                   state.cell_age_s);
        } else if (ok) {
            ok = append_json(buffer,
                             capacity,
                             &used,
                             "null,\"min_cell_battery\":null,"
                             "\"min_cell_number\":null,\"cell_age_s\":null");
        }
        ok = ok && append_json(buffer, capacity, &used, "}");
    }
    free(banks);
    ok = ok && append_json(buffer, capacity, &used, "]}");
    return finish_report(buffer, capacity, used, ok, json, length);
}

}  // namespace

const char *state_report_command_name(StateReportKind kind)
{
    switch (kind) {
    case StateReportKind::Batteries:
        return "batteries";
    case StateReportKind::Banks:
        return "banks";
    case StateReportKind::Relays:
        return "relays";
    case StateReportKind::Inputs:
        return "inputs";
    }
    return "unknown";
}

const char *state_report_frame_type(StateReportKind kind)
{
    switch (kind) {
    case StateReportKind::Batteries:
        return "report-batteries";
    case StateReportKind::Banks:
        return "report-banks";
    case StateReportKind::Relays:
        return "report-relays";
    case StateReportKind::Inputs:
        return "report-inputs";
    }
    return "report-unknown";
}

esp_err_t state_report_build(StateReportKind kind, char **json, size_t *length)
{
    if (json == nullptr || length == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *json = nullptr;
    *length = 0;
    switch (kind) {
    case StateReportKind::Batteries:
        return build_batteries(json, length);
    case StateReportKind::Banks:
        return build_banks(json, length);
    case StateReportKind::Relays:
        return build_relays(json, length);
    case StateReportKind::Inputs:
        return build_inputs(json, length);
    }
    return ESP_ERR_INVALID_ARG;
}
