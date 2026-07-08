#include "relay_manager.hpp"

#include <algorithm>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr const char *kTag = "relay_manager";
constexpr uint32_t kQueueLength = 16;
constexpr uint32_t kTaskStackBytes = 6144;
constexpr UBaseType_t kTaskPriority = 5;
constexpr TickType_t kRequestTimeout = pdMS_TO_TICKS(1000);

struct RelayState {
    bool timer_active = false;
    RelayForce force = RelayForce::None;
    bool output_on = false;
    int64_t off_at_us = 0;
};

enum class MessageType : uint8_t {
    OnFor,
    ForceOn,
    ForceOff,
    ClearForce,
    Query,
};

struct RelayMessage {
    MessageType type = MessageType::Query;
    uint8_t relay = 0;
    uint32_t seconds = 0;
    QueueHandle_t reply_queue = nullptr;
};

struct QueryReply {
    esp_err_t result = ESP_FAIL;
    RelayStatus status = {};
};

QueueHandle_t g_queue = nullptr;
StaticQueue_t g_queue_storage = {};
uint8_t g_queue_buffer[kQueueLength * sizeof(RelayMessage)] = {};

RelayState g_relays[CONFIG_POWER4_RELAY_COUNT] = {};
gpio_num_t g_relay_gpio[CONFIG_POWER4_RELAY_COUNT] = {};

bool valid_relay(uint8_t relay)
{
    return relay >= 1 && relay <= CONFIG_POWER4_RELAY_COUNT;
}

int64_t now_us(void)
{
    return esp_timer_get_time();
}

uint32_t remaining_seconds(int64_t now, int64_t deadline)
{
    if (deadline <= now) {
        return 0;
    }

    const uint64_t remaining_us = static_cast<uint64_t>(deadline - now);
    const uint64_t rounded_up_s = (remaining_us + 999999ULL) / 1000000ULL;
    return rounded_up_s > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(rounded_up_s);
}

RelayStatus make_status(uint8_t relay, const RelayState &state, int64_t now)
{
    RelayStatus status = {};
    status.relay = relay;
    status.gpio_pin = static_cast<int>(g_relay_gpio[relay - 1]);
    status.active_level = CONFIG_POWER4_RELAY_ACTIVE_LEVEL;
    status.timer_active = state.timer_active;
    status.force = state.force;
    status.output_on = state.output_on;
    if (state.timer_active) {
        status.timer_remaining_s = remaining_seconds(now, state.off_at_us);
    }
    return status;
}

int gpio_level_for(bool relay_on)
{
    if (relay_on) {
        return CONFIG_POWER4_RELAY_ACTIVE_LEVEL;
    }

    return CONFIG_POWER4_RELAY_ACTIVE_LEVEL == 0 ? 1 : 0;
}

esp_err_t parse_gpio_map(void)
{
    const char *cursor = CONFIG_POWER4_RELAY_GPIO_MAP;

    for (uint8_t relay = 1; relay <= CONFIG_POWER4_RELAY_COUNT; ++relay) {
        while (isspace(static_cast<unsigned char>(*cursor))) {
            ++cursor;
        }

        errno = 0;
        char *end = nullptr;
        const long pin = strtol(cursor, &end, 10);
        ESP_RETURN_ON_FALSE(end != cursor && errno == 0,
                            ESP_ERR_INVALID_ARG,
                            kTag,
                            "missing GPIO for relay %u in POWER4_RELAY_GPIO_MAP",
                            relay);
        ESP_RETURN_ON_FALSE(pin >= 0 && pin <= GPIO_NUM_MAX,
                            ESP_ERR_INVALID_ARG,
                            kTag,
                            "GPIO %ld for relay %u is out of range",
                            pin,
                            relay);
        ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(pin),
                            ESP_ERR_INVALID_ARG,
                            kTag,
                            "GPIO %ld for relay %u is not output-capable",
                            pin,
                            relay);

        g_relay_gpio[relay - 1] = static_cast<gpio_num_t>(pin);
        cursor = end;

        while (isspace(static_cast<unsigned char>(*cursor))) {
            ++cursor;
        }

        if (relay < CONFIG_POWER4_RELAY_COUNT) {
            ESP_RETURN_ON_FALSE(*cursor == ',',
                                ESP_ERR_INVALID_ARG,
                                kTag,
                                "expected comma after GPIO for relay %u",
                                relay);
            ++cursor;
        } else {
            ESP_RETURN_ON_FALSE(*cursor == '\0',
                                ESP_ERR_INVALID_ARG,
                                kTag,
                                "too many GPIOs in POWER4_RELAY_GPIO_MAP");
        }
    }

    return ESP_OK;
}

esp_err_t configure_relay_gpio(void)
{
    ESP_RETURN_ON_ERROR(parse_gpio_map(), kTag, "invalid relay GPIO map");

    uint64_t pin_mask = 0;
    for (uint8_t relay = 1; relay <= CONFIG_POWER4_RELAY_COUNT; ++relay) {
        pin_mask |= 1ULL << g_relay_gpio[relay - 1];
    }

    gpio_config_t io_config = {};
    io_config.pin_bit_mask = pin_mask;
    io_config.mode = GPIO_MODE_OUTPUT;
    io_config.pull_up_en = GPIO_PULLUP_DISABLE;
    io_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_config.intr_type = GPIO_INTR_DISABLE;

    ESP_RETURN_ON_ERROR(gpio_config(&io_config), kTag, "failed to configure relay GPIOs");

    for (uint8_t relay = 1; relay <= CONFIG_POWER4_RELAY_COUNT; ++relay) {
        ESP_RETURN_ON_ERROR(gpio_set_level(g_relay_gpio[relay - 1], gpio_level_for(false)),
                            kTag,
                            "failed to set relay %u GPIO inactive",
                            relay);
        ESP_LOGI(kTag,
                 "relay %u mapped to GPIO %d active_level=%d",
                 relay,
                 static_cast<int>(g_relay_gpio[relay - 1]),
                 CONFIG_POWER4_RELAY_ACTIVE_LEVEL);
    }

    return ESP_OK;
}

void apply_output(uint8_t relay, RelayState *state)
{
    bool desired_on = false;
    switch (state->force) {
    case RelayForce::On:
        desired_on = true;
        break;
    case RelayForce::Off:
        desired_on = false;
        break;
    case RelayForce::None:
        desired_on = state->timer_active;
        break;
    }

    if (state->output_on == desired_on) {
        return;
    }

    state->output_on = desired_on;

    const esp_err_t err = gpio_set_level(g_relay_gpio[relay - 1], gpio_level_for(desired_on));
    if (err != ESP_OK) {
        ESP_LOGE(kTag,
                 "failed to set relay %u GPIO %d: %s",
                 relay,
                 static_cast<int>(g_relay_gpio[relay - 1]),
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(kTag,
             "relay %u GPIO %d output %s (timer=%s force=%s)",
             relay,
             static_cast<int>(g_relay_gpio[relay - 1]),
             desired_on ? "on" : "off",
             state->timer_active ? "on" : "off",
             relay_force_name(state->force));
}

void expire_timers(int64_t now)
{
    for (uint8_t relay = 1; relay <= CONFIG_POWER4_RELAY_COUNT; ++relay) {
        RelayState &state = g_relays[relay - 1];
        if (state.timer_active && state.off_at_us <= now) {
            state.timer_active = false;
            state.off_at_us = 0;
            apply_output(relay, &state);
        }
    }
}

void handle_message(const RelayMessage &message)
{
    if (!valid_relay(message.relay)) {
        if (message.reply_queue != nullptr) {
            QueryReply reply = {};
            reply.result = ESP_ERR_INVALID_ARG;
            xQueueSend(message.reply_queue, &reply, 0);
        }
        return;
    }

    RelayState &state = g_relays[message.relay - 1];
    const int64_t now = now_us();

    switch (message.type) {
    case MessageType::OnFor: {
        const int64_t requested_off_at =
            now + (static_cast<int64_t>(message.seconds) * 1000000LL);
        state.off_at_us = state.timer_active ? std::max(state.off_at_us, requested_off_at)
                                             : requested_off_at;
        state.timer_active = message.seconds > 0;
        if (!state.timer_active) {
            state.off_at_us = 0;
        }
        apply_output(message.relay, &state);
        break;
    }

    case MessageType::ForceOn:
        state.force = RelayForce::On;
        apply_output(message.relay, &state);
        break;

    case MessageType::ForceOff:
        state.force = RelayForce::Off;
        apply_output(message.relay, &state);
        break;

    case MessageType::ClearForce:
        state.force = RelayForce::None;
        apply_output(message.relay, &state);
        break;

    case MessageType::Query:
        break;
    }

    if (message.reply_queue != nullptr) {
        QueryReply reply = {};
        reply.result = ESP_OK;
        reply.status = make_status(message.relay, state, now_us());
        xQueueSend(message.reply_queue, &reply, 0);
    }
}

void relay_task(void *arg)
{
    (void)arg;

    ESP_LOGI(kTag, "started with %u relays", CONFIG_POWER4_RELAY_COUNT);

    while (true) {
        RelayMessage message = {};
        if (xQueueReceive(g_queue, &message, pdMS_TO_TICKS(250)) == pdTRUE) {
            handle_message(message);
        }

        expire_timers(now_us());
    }
}

esp_err_t send_message(const RelayMessage &message)
{
    if (g_queue == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(g_queue, &message, kRequestTimeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t send_command(MessageType type, uint8_t relay, uint32_t seconds = 0)
{
    if (!valid_relay(relay)) {
        return ESP_ERR_INVALID_ARG;
    }

    RelayMessage message = {};
    message.type = type;
    message.relay = relay;
    message.seconds = seconds;

    return send_message(message);
}

}  // namespace

const char *relay_force_name(RelayForce force)
{
    switch (force) {
    case RelayForce::On:
        return "on";
    case RelayForce::Off:
        return "off";
    case RelayForce::None:
        break;
    }
    return "none";
}

uint8_t relay_manager_count(void)
{
    return CONFIG_POWER4_RELAY_COUNT;
}

esp_err_t relay_manager_start(void)
{
    if (g_queue != nullptr) {
        return ESP_OK;
    }

    memset(g_relays, 0, sizeof(g_relays));
    ESP_RETURN_ON_ERROR(configure_relay_gpio(), kTag, "failed to configure relay hardware");

    g_queue = xQueueCreateStatic(kQueueLength,
                                 sizeof(RelayMessage),
                                 g_queue_buffer,
                                 &g_queue_storage);
    ESP_RETURN_ON_FALSE(g_queue != nullptr, ESP_ERR_NO_MEM, kTag, "failed to create queue");

    BaseType_t ok = xTaskCreate(relay_task,
                                "relay_manager",
                                kTaskStackBytes,
                                nullptr,
                                kTaskPriority,
                                nullptr);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, kTag, "failed to create task");

    return ESP_OK;
}

esp_err_t relay_manager_on_for(uint8_t relay, uint32_t seconds)
{
    return send_command(MessageType::OnFor, relay, seconds);
}

esp_err_t relay_manager_force_on(uint8_t relay)
{
    return send_command(MessageType::ForceOn, relay);
}

esp_err_t relay_manager_force_off(uint8_t relay)
{
    return send_command(MessageType::ForceOff, relay);
}

esp_err_t relay_manager_clear_force(uint8_t relay)
{
    return send_command(MessageType::ClearForce, relay);
}

esp_err_t relay_manager_query(uint8_t relay, RelayStatus *status)
{
    if (status == nullptr || !valid_relay(relay)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_queue == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    StaticQueue_t reply_queue_storage = {};
    uint8_t reply_queue_buffer[sizeof(QueryReply)] = {};
    QueueHandle_t reply_queue = xQueueCreateStatic(1,
                                                   sizeof(QueryReply),
                                                   reply_queue_buffer,
                                                   &reply_queue_storage);
    if (reply_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    RelayMessage message = {};
    message.type = MessageType::Query;
    message.relay = relay;
    message.reply_queue = reply_queue;

    esp_err_t result = send_message(message);
    if (result == ESP_OK) {
        QueryReply reply = {};
        if (xQueueReceive(reply_queue, &reply, kRequestTimeout) == pdTRUE) {
            result = reply.result;
            if (result == ESP_OK) {
                *status = reply.status;
            }
        } else {
            result = ESP_ERR_TIMEOUT;
        }
    }

    vQueueDelete(reply_queue);
    return result;
}
