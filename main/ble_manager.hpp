#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t ble_manager_start(void);
bool ble_manager_is_synced(void);
esp_err_t ble_manager_wait_until_synced(TickType_t timeout);
// Called by the battery scanner task after exhausted cleanup. Resets all BLE
// links and waits for a fresh synchronization. A timeout leaves recovery pending;
// a later call joins that recovery instead of scheduling another reset.
esp_err_t ble_manager_reset(TickType_t timeout);
uint8_t ble_manager_own_addr_type(void);
esp_err_t ble_manager_restart_advertising(void);
