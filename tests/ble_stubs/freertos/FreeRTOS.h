#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
typedef int BaseType_t;
#define BIT0 1U
#define BIT1 2U
#define pdFALSE 0
#define pdTRUE 1
