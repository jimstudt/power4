#pragma once

#include <stddef.h>

#include "esp_err.h"

constexpr size_t kLogBufferBytes = 16384;

// Hook system logging and keep the most recent kLogBufferBytes of log text
// in a rolling buffer. Log output still reaches the console as before.
esp_err_t log_buffer_init(void);
// Copy the most recent buffered log text into dst, newest text last. When
// dst_size is smaller than the buffered text the oldest text is dropped.
// Returns the number of bytes copied; the result is not null terminated.
size_t log_buffer_snapshot(char *dst, size_t dst_size);
