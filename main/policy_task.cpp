#include "policy_task.hpp"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "battery_bank.hpp"
#include "config_flags.hpp"
#include "policy_storage.hpp"
#include "relay_manager.hpp"
#include "esp_err.h"
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

void log_lua_error(lua_State *state, const char *phase)
{
    const char *message = lua_tostring(state, -1);
    if (message == nullptr) {
        message = "unknown Lua error";
    }
    ESP_LOGW(kTag, "policy %s failed: %s", phase, message);
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
    const esp_err_t err = relay_manager_on_for(relay, kRelayPolicyHoldSeconds);
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
    lua_pushboolean(state, status.forced_on);
    lua_pushinteger(state, static_cast<lua_Integer>(status.timer_remaining_s));
    return 3;
}

int lua_config_is_set(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);

    bool is_set = false;
    const esp_err_t err = config_flags_is_set(name, &is_set);
    if (err != ESP_OK) {
        return luaL_error(state, "config_is_set(%s) failed: %s", name, esp_err_to_name(err));
    }

    lua_pushboolean(state, is_set);
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
        lua_pushnil(state);
        lua_pushnil(state);
        lua_pushnil(state);
        return 4;
    }

    lua_pushnumber(state, bank_state.voltage_v);
    lua_pushnumber(state, bank_state.current_a);
    lua_pushnumber(state, bank_state.soc_percent);
    return 4;
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

    ESP_LOGI(kTag, "%s", message);
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
    lua_pushcfunction(state, lua_config_is_set);
    lua_setglobal(state, "config_is_set");
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
        ESP_LOGE(kTag, "failed to create Lua state");
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
        ESP_LOGW(kTag, "policy flag expiry failed: %s", esp_err_to_name(expire_err));
    }

    char *active_source = nullptr;
    size_t active_length = 0;
    esp_err_t err = policy_storage_read_alloc(PolicySlot::Active, &active_source, &active_length);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "failed to read active policy: %s", esp_err_to_name(err));
        return;
    }

    const bool has_active_policy = active_length > 0;
    const char *source = has_active_policy ? active_source : kEmptyPolicySource;
    const size_t length = has_active_policy ? active_length : strlen(kEmptyPolicySource);
    const char *chunk_name = has_active_policy ? "policy_active" : "policy_empty";

    ESP_LOGI(kTag,
             "running %s policy (%u bytes)",
             has_active_policy ? "active" : "empty",
             static_cast<unsigned>(length));
    if (!run_lua_policy(source, length, chunk_name, deadline)) {
        ESP_LOGW(kTag, "policy cycle did not complete successfully");
    }

    free(active_source);
}

void policy_task_main(void *arg)
{
    (void)arg;

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
