#pragma once

#include "esp_err.h"

extern "C" {
#include "host/ble_uuid.h"
}

esp_err_t relay_gatt_register(void);
const ble_uuid128_t *relay_gatt_service_uuid(void);
