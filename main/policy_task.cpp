#include "policy_task.hpp"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "battery_bank.hpp"
#include "config_flags.hpp"
#include "input_manager.hpp"
#include "policy_storage.hpp"
#include "relay_manager.hpp"
#include "rtc_manager.hpp"
#include "time_manager.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

extern "C" {
#define LUA_32BITS 1
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace {

constexpr const char *kTag = "policy_task";
constexpr const char *kTaskName = "policy";
constexpr TickType_t kPolicyPeriodTicks = pdMS_TO_TICKS(CONFIG_POWER4_POLICY_PERIOD_SECONDS * 1000);
constexpr int kLuaHookInstructionCount = 1000;
constexpr uint32_t kRelayPolicyHoldSeconds = 300;
constexpr lua_Integer kRelayPolicyHoldMaxSeconds = 86400;
constexpr size_t kLuaSyslogMessageBytes = 192;
constexpr const char *kEmptyPolicySource =
    "syslog(\"power4 policy: no active configuration\")\n";

struct LuaRunContext {
    TickType_t deadline;
};

TaskHandle_t g_policy_task = nullptr;

bool tick_reached(TickType_t deadline)
{
    return static_cast<int32_t>(xTaskGetTickCount() - deadline) >= 0;
}

void policy_lua_hook(lua_State *state, lua_Debug *debug)
{
    (void)debug;

    LuaRunContext *context = *static_cast<LuaRunContext **>(lua_getextraspace(state));
    if (context != nullptr && tick_reached(context->deadline)) {
        luaL_error(state, "policy execution timed out");
    }
}

// Single emit point for policy syslog lines; Lua's syslog() and policy
// error reporting share it so log scrapers see one consistent stream.
void policy_syslog(const char *message)
{
    ESP_LOGI(kTag, "%s", message);
}

void log_heap_failure(const char *operation)
{
    constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGE(kTag,
             "%s: internal_free=%u internal_largest=%u",
             operation,
             static_cast<unsigned>(heap_caps_get_free_size(caps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(caps)));
}

void log_lua_error(lua_State *state, const char *phase)
{
    const char *message = lua_tostring(state, -1);
    if (message == nullptr) {
        message = "unknown Lua error";
    }

    char line[kLuaSyslogMessageBytes] = {};
    snprintf(line, sizeof(line), "policy error (%s): %s", phase, message);
    policy_syslog(line);
}

uint8_t lua_check_relay(lua_State *state, int arg)
{
    const lua_Integer relay = luaL_checkinteger(state, arg);
    if (relay < 1 || relay > relay_manager_count()) {
        luaL_argerror(state, arg, "relay number out of range");
    }

    return static_cast<uint8_t>(relay);
}

int lua_relay_on(lua_State *state)
{
    const uint8_t relay = lua_check_relay(state, 1);

    uint32_t hold_seconds = kRelayPolicyHoldSeconds;
    if (!lua_isnoneornil(state, 2)) {
        const lua_Integer seconds = luaL_checkinteger(state, 2);
        if (seconds < 1 || seconds > kRelayPolicyHoldMaxSeconds) {
            luaL_argerror(state, 2, "hold seconds out of range");
        }
        hold_seconds = static_cast<uint32_t>(seconds);
    }

    const esp_err_t err = relay_manager_on_for(relay, hold_seconds);
    if (err != ESP_OK) {
        return luaL_error(state, "relay_on(%u) failed: %s", relay, esp_err_to_name(err));
    }

    return 0;
}

int lua_relay_off(lua_State *state)
{
    const uint8_t relay = lua_check_relay(state, 1);
    const esp_err_t err = relay_manager_on_for(relay, 0);
    if (err != ESP_OK) {
        return luaL_error(state, "relay_off(%u) failed: %s", relay, esp_err_to_name(err));
    }

    return 0;
}

int lua_relay_state(lua_State *state)
{
    const uint8_t relay = lua_check_relay(state, 1);

    RelayStatus status = {};
    const esp_err_t err = relay_manager_query(relay, &status);
    if (err != ESP_OK) {
        return luaL_error(state, "relay_state(%u) failed: %s", relay, esp_err_to_name(err));
    }

    lua_pushboolean(state, status.output_on);
    if (status.force == RelayForce::None) {
        // nil when no administrative force, "on"/"off" otherwise, so
        // `if force then` still answers "is a force active" and the
        // string answers which direction it holds.
        lua_pushnil(state);
    } else {
        lua_pushstring(state, relay_force_name(status.force));
    }
    lua_pushinteger(state, static_cast<lua_Integer>(status.timer_remaining_s));
    return 3;
}

int lua_input_on(lua_State *state)
{
    const lua_Integer input = luaL_checkinteger(state, 1);
    if (input < 1 || input > input_manager_count()) {
        luaL_argerror(state, 1, "input number out of range");
    }

    InputStatus status = {};
    const esp_err_t err = input_manager_query(static_cast<uint8_t>(input), &status);
    if (err != ESP_OK) {
        return luaL_error(state,
                          "input_on(%u) failed: %s",
                          static_cast<unsigned>(input),
                          esp_err_to_name(err));
    }

    lua_pushboolean(state, status.on);
    return 1;
}

void lua_set_integer_field(lua_State *state, const char *name, lua_Integer value)
{
    lua_pushinteger(state, value);
    lua_setfield(state, -2, name);
}

int lua_rtc_time(lua_State *state)
{
    RtcDateTime value = {};
    const esp_err_t err = rtc_manager_read(&value);
    if (err != ESP_OK) {
        return luaL_error(state, "rtc_time() failed: %s", esp_err_to_name(err));
    }

    lua_createtable(state, 0, 10);
    lua_set_integer_field(state, "year", value.year);
    lua_set_integer_field(state, "month", value.month);
    lua_set_integer_field(state, "day", value.day);
    lua_set_integer_field(state, "weekday", value.weekday);
    lua_set_integer_field(state, "hour", value.hour);
    lua_set_integer_field(state, "minute", value.minute);
    lua_set_integer_field(state, "second", value.second);
    lua_pushboolean(state, !value.oscillator_stopped);
    lua_setfield(state, -2, "valid");
    lua_pushboolean(state, value.oscillator_stopped);
    lua_setfield(state, -2, "oscillator_stopped");
    lua_pushboolean(state, true);
    lua_setfield(state, -2, "utc");
    return 1;
}

int lua_local_time(lua_State *state)
{
    TimeSnapshot value = {};
    const esp_err_t err = time_manager_get_time(&value);
    if (err != ESP_OK) {
        return luaL_error(state, "local_time() failed: %s", esp_err_to_name(err));
    }

    lua_createtable(state, 0, 14);
    lua_set_integer_field(state, "year", value.local.tm_year + 1900);
    lua_set_integer_field(state, "month", value.local.tm_mon + 1);
    lua_set_integer_field(state, "day", value.local.tm_mday);
    lua_set_integer_field(state, "weekday", value.local.tm_wday);
    lua_set_integer_field(state, "yearday", value.local.tm_yday + 1);
    lua_set_integer_field(state, "hour", value.local.tm_hour);
    lua_set_integer_field(state, "minute", value.local.tm_min);
    lua_set_integer_field(state, "second", value.local.tm_sec);
    lua_set_integer_field(state, "utc_offset_minutes", value.utc_offset_minutes);
    lua_pushboolean(state, value.valid);
    lua_setfield(state, -2, "valid");
    lua_pushboolean(state, value.local.tm_isdst > 0);
    lua_setfield(state, -2, "daylight_saving");
    lua_pushboolean(state, false);
    lua_setfield(state, -2, "utc");
    lua_pushstring(state, value.abbreviation);
    lua_setfield(state, -2, "zone");
    lua_pushstring(state, value.timezone_name);
    lua_setfield(state, -2, "timezone");
    return 1;
}

int lua_config_is_set(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    if (!config_flags_valid_name(name)) {
        // An impossible name can never be set, so answer false rather than
        // killing the policy run, but leave a trail for the policy author.
        char line[kLuaSyslogMessageBytes] = {};
        snprintf(line,
                 sizeof(line),
                 "config_is_set(%s): impossible parameter name "
                 "(1-15 characters: letters, digits, '_', '-'), returning false",
                 name);
        policy_syslog(line);
        lua_pushboolean(state, false);
        return 1;
    }

    bool is_set = false;
    const esp_err_t err = config_flags_is_set(name, &is_set);
    if (err != ESP_OK) {
        return luaL_error(state, "config_is_set(%s) failed: %s", name, esp_err_to_name(err));
    }

    lua_pushboolean(state, is_set);
    return 1;
}

// Push the caller's optional default (argument 2) or nil.
int lua_push_config_default(lua_State *state)
{
    if (lua_gettop(state) >= 2) {
        lua_pushvalue(state, 2);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

// Syslog why a config read is answering the default rather than killing
// the policy run, leaving a trail for the policy author.
void syslog_config_default(const char *function, const char *name, const char *why)
{
    char line[kLuaSyslogMessageBytes] = {};
    snprintf(line, sizeof(line), "%s(%s): %s, returning default", function, name, why);
    policy_syslog(line);
}

int lua_config_number(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    if (!config_flags_valid_name(name)) {
        syslog_config_default("config_number",
                              name,
                              "impossible parameter name "
                              "(1-15 characters: letters, digits, '_', '-')");
        return lua_push_config_default(state);
    }

    char text[kConfigNumberTextMaxBytes] = {};
    bool found = false;
    const esp_err_t err = config_flags_get_number(name, text, sizeof(text), &found);
    if (err != ESP_OK) {
        return luaL_error(state, "config_number(%s) failed: %s", name, esp_err_to_name(err));
    }

    if (!found) {
        // A boolean under this name is a likely policy bug; say so.
        ConfigFlagType type = ConfigFlagType::NotSet;
        if (config_flags_type(name, &type) == ESP_OK && type == ConfigFlagType::Boolean) {
            syslog_config_default("config_number", name, "holds a boolean");
        }
        return lua_push_config_default(state);
    }

    ConfigNumber number = {};
    if (!config_number_parse(text, &number)) {
        syslog_config_default("config_number", name, "stored value is not numeric");
        return lua_push_config_default(state);
    }

    if (number.is_integer) {
        lua_pushinteger(state, static_cast<lua_Integer>(number.integer));
    } else {
        lua_pushnumber(state, static_cast<lua_Number>(number.value));
    }
    return 1;
}

int lua_config_bool(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    if (!config_flags_valid_name(name)) {
        syslog_config_default("config_bool",
                              name,
                              "impossible parameter name "
                              "(1-15 characters: letters, digits, '_', '-')");
        return lua_push_config_default(state);
    }

    bool value = false;
    bool found = false;
    const esp_err_t err = config_flags_get_bool(name, &value, &found);
    if (err != ESP_OK) {
        return luaL_error(state, "config_bool(%s) failed: %s", name, esp_err_to_name(err));
    }

    if (!found) {
        // A number under this name is a likely policy bug; say so.
        ConfigFlagType type = ConfigFlagType::NotSet;
        if (config_flags_type(name, &type) == ESP_OK && type == ConfigFlagType::Number) {
            syslog_config_default("config_bool", name, "holds a number");
        }
        return lua_push_config_default(state);
    }

    lua_pushboolean(state, value);
    return 1;
}

int lua_battery_bank_state(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);

    BatteryBankState bank_state = {};
    const esp_err_t err = battery_bank_get_state(name, &bank_state);
    if (err != ESP_OK) {
        return luaL_error(state, "battery_bank_state(%s) failed: %s", name, esp_err_to_name(err));
    }

    lua_pushboolean(state, bank_state.ready);
    if (!bank_state.ready) {
        for (int i = 0; i < 6; ++i) {
            lua_pushnil(state);
        }
        return 7;
    }

    lua_pushnumber(state, bank_state.voltage_v);
    lua_pushnumber(state, bank_state.current_a);
    lua_pushnumber(state, bank_state.soc_percent);
    if (bank_state.cell_data_ready) {
        lua_pushnumber(state, bank_state.min_cell_voltage_v);
        lua_pushinteger(state, static_cast<lua_Integer>(bank_state.cell_age_s));
    } else {
        lua_pushnil(state);
        lua_pushnil(state);
    }
    lua_pushboolean(state, bank_state.cell_undervoltage_protection);
    return 7;
}

int lua_battery_bank_names(lua_State *state)
{
    BatteryBankList *banks = static_cast<BatteryBankList *>(malloc(sizeof(BatteryBankList)));
    if (banks == nullptr) {
        return luaL_error(state, "battery_bank_names() failed: out of memory");
    }

    const esp_err_t err = battery_bank_list(banks);
    if (err != ESP_OK) {
        free(banks);
        return luaL_error(state, "battery_bank_names() failed: %s", esp_err_to_name(err));
    }

    lua_createtable(state, static_cast<int>(banks->count), 0);
    for (size_t i = 0; i < banks->count; ++i) {
        lua_pushstring(state, banks->banks[i].name);
        lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
    }

    free(banks);
    return 1;
}

int lua_syslog(lua_State *state)
{
    char message[kLuaSyslogMessageBytes] = {};
    size_t used = 0;
    const int argc = lua_gettop(state);

    for (int i = 1; i <= argc; ++i) {
        if (i > 1 && used + 1 < sizeof(message)) {
            message[used] = '\t';
            ++used;
            message[used] = '\0';
        }

        size_t length = 0;
        const char *text = luaL_tolstring(state, i, &length);
        if (text == nullptr) {
            text = "";
            length = 0;
        }

        const size_t remaining = sizeof(message) - used - 1;
        const size_t copy_length = length < remaining ? length : remaining;
        if (copy_length > 0) {
            memcpy(message + used, text, copy_length);
            used += copy_length;
            message[used] = '\0';
        }
        lua_pop(state, 1);
    }

    policy_syslog(message);
    return 0;
}

void register_policy_lua_functions(lua_State *state)
{
    lua_pushcfunction(state, lua_relay_on);
    lua_setglobal(state, "relay_on");
    lua_pushcfunction(state, lua_relay_off);
    lua_setglobal(state, "relay_off");
    lua_pushcfunction(state, lua_relay_state);
    lua_setglobal(state, "relay_state");
    lua_pushcfunction(state, lua_input_on);
    lua_setglobal(state, "input_on");
    lua_pushcfunction(state, lua_rtc_time);
    lua_setglobal(state, "rtc_time");
    lua_pushcfunction(state, lua_local_time);
    lua_setglobal(state, "local_time");
    lua_pushcfunction(state, lua_config_is_set);
    lua_setglobal(state, "config_is_set");
    lua_pushcfunction(state, lua_config_number);
    lua_setglobal(state, "config_number");
    lua_pushcfunction(state, lua_config_bool);
    lua_setglobal(state, "config_bool");
    lua_pushcfunction(state, lua_battery_bank_state);
    lua_setglobal(state, "battery_bank_state");
    lua_pushcfunction(state, lua_battery_bank_names);
    lua_setglobal(state, "battery_bank_names");
    lua_pushcfunction(state, lua_syslog);
    lua_setglobal(state, "syslog");
}

void open_policy_lua_libraries(lua_State *state)
{
    luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(state, 1);
}

bool run_lua_policy(const char *source, size_t length, const char *chunk_name, TickType_t deadline)
{
    lua_State *state = luaL_newstate();
    if (state == nullptr) {
        log_heap_failure("failed to create Lua state");
        return false;
    }

    LuaRunContext context = {
        .deadline = deadline,
    };
    *static_cast<LuaRunContext **>(lua_getextraspace(state)) = &context;

    open_policy_lua_libraries(state);
    register_policy_lua_functions(state);
    lua_sethook(state, policy_lua_hook, LUA_MASKCOUNT, kLuaHookInstructionCount);

    bool ok = false;
    if (luaL_loadbuffer(state, source, length, chunk_name) != LUA_OK) {
        log_lua_error(state, "load");
    } else if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
        log_lua_error(state, "run");
    } else {
        ok = true;
    }

    lua_close(state);
    return ok;
}

void run_policy_cycle(TickType_t deadline)
{
    const esp_err_t expire_err = config_flags_expire();
    if (expire_err != ESP_OK) {
        ESP_LOGW(kTag, "policy parameter expiry failed: %s", esp_err_to_name(expire_err));
    }

    char *active_source = nullptr;
    size_t active_length = 0;
    esp_err_t err = policy_storage_read_alloc(PolicySlot::Active, &active_source, &active_length);
    if (err != ESP_OK) {
        ESP_LOGW(kTag,
                 "failed to read active policy: %s internal_free=%u internal_largest=%u",
                 esp_err_to_name(err),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                               MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        return;
    }

    const bool has_active_policy = active_length > 0;
    const char *source = has_active_policy ? active_source : kEmptyPolicySource;
    const size_t length = has_active_policy ? active_length : strlen(kEmptyPolicySource);
    const char *chunk_name = has_active_policy ? "policy_active" : "policy_empty";

    if (!run_lua_policy(source, length, chunk_name, deadline)) {
        ESP_LOGW(kTag, "policy cycle did not complete successfully");
    }

    free(active_source);
}

void policy_task_main(void *arg)
{
    (void)arg;

    // The policy task has a deliberately large stack and Lua also needs a
    // sizeable contiguous heap block. Let app_main finish and release its
    // startup stack before the first Lua state is created.
    vTaskDelay(pdMS_TO_TICKS(100));

    while (true) {
        const TickType_t cycle_start = xTaskGetTickCount();
        const TickType_t deadline = cycle_start + kPolicyPeriodTicks;

        run_policy_cycle(deadline);

        const TickType_t now = xTaskGetTickCount();
        const TickType_t next_cycle = cycle_start + kPolicyPeriodTicks;
        if (!tick_reached(next_cycle)) {
            vTaskDelay(next_cycle - now);
        } else {
            taskYIELD();
        }
    }
}

}  // namespace

esp_err_t policy_validate(const char *source,
                          size_t length,
                          char *error_message,
                          size_t error_message_size)
{
    if (error_message != nullptr && error_message_size > 0) {
        error_message[0] = '\0';
    }
    if (source == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    lua_State *state = luaL_newstate();
    if (state == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    *static_cast<LuaRunContext **>(lua_getextraspace(state)) = nullptr;

    esp_err_t result = ESP_OK;
    if (luaL_loadbuffer(state, source, length, "policy_staged") != LUA_OK) {
        const char *message = lua_tostring(state, -1);
        if (message == nullptr) {
            message = "unknown Lua error";
        }
        if (error_message != nullptr && error_message_size > 0) {
            strlcpy(error_message, message, error_message_size);
        }
        result = ESP_FAIL;
    }

    lua_close(state);
    return result;
}

esp_err_t policy_task_start(void)
{
    if (g_policy_task != nullptr) {
        return ESP_OK;
    }

    BaseType_t created = xTaskCreate(policy_task_main,
                                     kTaskName,
                                     CONFIG_POWER4_POLICY_TASK_STACK_BYTES,
                                     nullptr,
                                     CONFIG_POWER4_POLICY_TASK_PRIORITY,
                                     &g_policy_task);
    if (created != pdPASS) {
        g_policy_task = nullptr;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
