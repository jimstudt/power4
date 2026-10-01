#pragma once

#include <stdint.h>

typedef struct {
    uint8_t type;
    uint8_t value[16];
} ble_uuid128_t;
