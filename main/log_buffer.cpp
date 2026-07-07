#include "log_buffer.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log_write.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {

constexpr size_t kLineMaxBytes = 256;
constexpr TickType_t kAppendTimeoutTicks = pdMS_TO_TICKS(100);

char g_buffer[kLogBufferBytes];
size_t g_length = 0;
vprintf_like_t g_previous_vprintf = nullptr;
StaticSemaphore_t g_mutex_storage;
SemaphoreHandle_t g_mutex = nullptr;

void append_locked(const char *text, size_t length)
{
    if (length > sizeof(g_buffer)) {
        text += length - sizeof(g_buffer);
        length = sizeof(g_buffer);
    }

    if (g_length + length > sizeof(g_buffer)) {
        const size_t shift = g_length + length - sizeof(g_buffer);
        memmove(g_buffer, g_buffer + shift, g_length - shift);
        g_length -= shift;
    }

    memcpy(g_buffer + g_length, text, length);
    g_length += length;
}

int log_buffer_vprintf(const char *format, va_list args)
{
    char line[kLineMaxBytes];
    va_list copy;
    va_copy(copy, args);
    const int formatted = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);

    if (formatted > 0 && g_mutex != nullptr) {
        size_t length = static_cast<size_t>(formatted);
        if (length >= sizeof(line)) {
            // Truncated: keep the line ending so entries stay separated.
            length = sizeof(line) - 1;
            line[length - 1] = '\n';
        }
        // A logger that cannot take the mutex promptly drops its copy
        // rather than stalling the logging task.
        if (xSemaphoreTake(g_mutex, kAppendTimeoutTicks) == pdTRUE) {
            append_locked(line, length);
            xSemaphoreGive(g_mutex);
        }
    }

    if (g_previous_vprintf != nullptr) {
        return g_previous_vprintf(format, args);
    }
    return formatted;
}

}  // namespace

esp_err_t log_buffer_init(void)
{
    if (g_mutex != nullptr) {
        return ESP_OK;
    }

    g_mutex = xSemaphoreCreateMutexStatic(&g_mutex_storage);
    if (g_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    g_previous_vprintf = esp_log_set_vprintf(&log_buffer_vprintf);
    return ESP_OK;
}

size_t log_buffer_snapshot(char *dst, size_t dst_size)
{
    if (dst == nullptr || dst_size == 0 || g_mutex == nullptr) {
        return 0;
    }

    size_t copied = 0;
    if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
        copied = g_length < dst_size ? g_length : dst_size;
        memcpy(dst, g_buffer + (g_length - copied), copied);
        xSemaphoreGive(g_mutex);
    }
    return copied;
}
