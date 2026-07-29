#include "relay_manager.hpp"

#include <algorithm>
#include <inttypes.h>
#include <string.h>

#include "board_i2c.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
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
constexpr int kI2cTimeoutMs = 100;

constexpr uint8_t kTcaOutputRegister = 0x01;
constexpr uint8_t kTcaConfigurationRegister = 0x03;

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

RelayState g_relays[CONFIG_POWER4_MAX_RELAYS] = {};
RelayHardwareConfig g_hardware = {};
i2c_master_dev_handle_t g_tca9554 = nullptr;
uint8_t g_tca_output = 0;

bool valid_relay(uint8_t relay)
{
    return relay >= 1 && relay <= g_hardware.count;
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

int output_level_for(bool relay_on)
{
    if (relay_on) {
        return g_hardware.active_level;
    }
    return g_hardware.active_level == 0 ? 1 : 0;
}

RelayStatus make_status(uint8_t relay, const RelayState &state, int64_t now)
{
    RelayStatus status = {};
    status.relay = relay;
    status.backend = g_hardware.kind;
    status.hardware_channel = g_hardware.channels[relay - 1];
    if (g_hardware.kind == RelayBackendKind::Gpio) {
        status.gpio_pin = status.hardware_channel;
    }
    status.active_level = g_hardware.active_level;
    status.timer_active = state.timer_active;
    status.force = state.force;
    status.output_on = state.output_on;
    if (state.timer_active) {
        status.timer_remaining_s = remaining_seconds(now, state.off_at_us);
    }
    return status;
}

esp_err_t tca_write_register(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(g_tca9554, data, sizeof(data), kI2cTimeoutMs);
}

esp_err_t configure_gpio_backend(void)
{
    uint64_t pin_mask = 0;
    for (uint8_t i = 0; i < g_hardware.count; ++i) {
        const gpio_num_t gpio = static_cast<gpio_num_t>(g_hardware.channels[i]);
        ESP_RETURN_ON_ERROR(gpio_set_level(gpio, output_level_for(false)),
                            kTag,
                            "failed to preload relay %u GPIO inactive",
                            i + 1);
        pin_mask |= 1ULL << gpio;
    }

    gpio_config_t io_config = {};
    io_config.pin_bit_mask = pin_mask;
    io_config.mode = GPIO_MODE_OUTPUT;
    io_config.pull_up_en = GPIO_PULLUP_DISABLE;
    io_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io_config), kTag, "failed to configure relay GPIOs");

    for (uint8_t i = 0; i < g_hardware.count; ++i) {
        ESP_LOGI(kTag,
                 "relay %u mapped to GPIO %d active_level=%u",
                 i + 1,
                 g_hardware.channels[i],
                 g_hardware.active_level);
    }
    return ESP_OK;
}

esp_err_t configure_tca9554_backend(uint32_t i2c_frequency_hz)
{
    ESP_RETURN_ON_ERROR(board_i2c_add_device(g_hardware.i2c_address,
                                              i2c_frequency_hz,
                                              &g_tca9554),
                        kTag,
                        "failed to attach TCA9554");

    g_tca_output = g_hardware.active_level == 0 ? 0xff : 0x00;
    ESP_RETURN_ON_ERROR(tca_write_register(kTcaOutputRegister, g_tca_output),
                        kTag,
                        "failed to drive TCA9554 relays inactive");
    ESP_RETURN_ON_ERROR(tca_write_register(kTcaConfigurationRegister, 0x00),
                        kTag,
                        "failed to configure TCA9554 outputs");

    for (uint8_t i = 0; i < g_hardware.count; ++i) {
        ESP_LOGI(kTag,
                 "relay %u mapped to TCA9554 0x%02x bit %d active_level=%u",
                 i + 1,
                 g_hardware.i2c_address,
                 g_hardware.channels[i],
                 g_hardware.active_level);
    }
    return ESP_OK;
}

esp_err_t hardware_set_output(uint8_t relay, bool output_on)
{
    const int channel = g_hardware.channels[relay - 1];
    if (g_hardware.kind == RelayBackendKind::Gpio) {
        return gpio_set_level(static_cast<gpio_num_t>(channel), output_level_for(output_on));
    }

    if (g_hardware.kind == RelayBackendKind::Tca9554) {
        const uint8_t mask = static_cast<uint8_t>(1U << channel);
        uint8_t next = g_tca_output;
        if (output_level_for(output_on) != 0) {
            next |= mask;
        } else {
            next &= static_cast<uint8_t>(~mask);
        }
        esp_err_t err = tca_write_register(kTcaOutputRegister, next);
        if (err == ESP_OK) {
            g_tca_output = next;
        }
        return err;
    }

    return ESP_ERR_NOT_SUPPORTED;
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

    const esp_err_t err = hardware_set_output(relay, desired_on);
    if (err != ESP_OK) {
        ESP_LOGE(kTag,
                 "failed to set relay %u through %s channel %d: %s",
                 relay,
                 relay_backend_name(g_hardware.kind),
                 g_hardware.channels[relay - 1],
                 esp_err_to_name(err));
        return;
    }

    state->output_on = desired_on;
    ESP_LOGI(kTag,
             "relay %u output %s via %s channel %d (timer=%s force=%s)",
             relay,
             desired_on ? "on" : "off",
             relay_backend_name(g_hardware.kind),
             g_hardware.channels[relay - 1],
             state->timer_active ? "on" : "off",
             relay_force_name(state->force));
}

void expire_timers(int64_t now)
{
    for (uint8_t relay = 1; relay <= g_hardware.count; ++relay) {
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
    ESP_LOGI(kTag,
             "started with %u relays using %s",
             g_hardware.count,
             relay_backend_name(g_hardware.kind));

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
    return xQueueSend(g_queue, &message, kRequestTimeout) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
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
    return g_hardware.count;
}

esp_err_t relay_manager_start(const BoardConfig &board)
{
    if (g_queue != nullptr) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(board.relay.count <= CONFIG_POWER4_MAX_RELAYS,
                        ESP_ERR_INVALID_SIZE,
                        kTag,
                        "board relay count exceeds firmware capacity");

    memset(g_relays, 0, sizeof(g_relays));
    g_hardware = board.relay;
    if (g_hardware.kind == RelayBackendKind::Gpio) {
        ESP_RETURN_ON_ERROR(configure_gpio_backend(), kTag, "failed to configure GPIO relays");
    } else if (g_hardware.kind == RelayBackendKind::Tca9554) {
        ESP_RETURN_ON_ERROR(configure_tca9554_backend(board.i2c.frequency_hz),
                            kTag,
                            "failed to configure TCA9554 relays");
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

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
