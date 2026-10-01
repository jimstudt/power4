#include "ble_disconnect.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "host/ble_hs.h"
}

namespace {

constexpr const char *kTag = "ble_disconnect";
constexpr unsigned kDisconnectAttempts = 3;
constexpr TickType_t kDisconnectTimeoutTicks = pdMS_TO_TICKS(5000);
constexpr TickType_t kRetryDelayTicks = pdMS_TO_TICKS(250);

}  // namespace

bool ble_disconnect_peer(const ble_addr_t &addr)
{
    for (unsigned attempt = 1; attempt <= kDisconnectAttempts; ++attempt) {
        if (attempt > 1) {
            vTaskDelay(kRetryDelayTicks);
        }

        // Looking up the peer also covers a link established before NimBLE has
        // delivered its CONNECT callback (and therefore its handle) to us.
        ble_gap_conn_desc conn = {};
        const int find_rc = ble_gap_conn_find_by_addr(&addr, &conn);
        if (find_rc == BLE_HS_ENOTCONN) {
            return true;
        }
        if (find_rc != 0) {
            ESP_LOGW(kTag, "cannot check peer connection: attempt=%u/%u rc=%d",
                     attempt, kDisconnectAttempts, find_rc);
            continue;
        }

        const int rc = ble_gap_terminate(conn.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0 && rc != BLE_HS_EALREADY && rc != BLE_HS_ENOTCONN) {
            ESP_LOGW(kTag, "disconnect request failed: handle=%u attempt=%u/%u rc=%d",
                     static_cast<unsigned>(conn.conn_handle), attempt, kDisconnectAttempts, rc);
            continue;
        }

        // EALREADY means termination is pending, not that the link is gone.
        // Poll the host's synchronized connection table so a missed callback or
        // a stale event bit cannot make cleanup falsely report success.
        const TickType_t started = xTaskGetTickCount();
        while (true) {
            const int check_rc = ble_gap_conn_find_by_addr(&addr, nullptr);
            if (check_rc == BLE_HS_ENOTCONN) {
                return true;
            }
            if (check_rc != 0 ||
                xTaskGetTickCount() - started >= kDisconnectTimeoutTicks) {
                break;
            }
            vTaskDelay(kRetryDelayTicks);
        }
        ESP_LOGW(kTag, "disconnect not confirmed: handle=%u attempt=%u/%u rc=%d",
                 static_cast<unsigned>(conn.conn_handle), attempt, kDisconnectAttempts, rc);
    }

    // A disconnect can complete during the final failed request as well.
    return ble_gap_conn_find_by_addr(&addr, nullptr) == BLE_HS_ENOTCONN;
}
