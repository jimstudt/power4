#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ble_disconnect.hpp"
#include "freertos/task.h"

extern "C" {
#include "host/ble_hs.h"
}

namespace {

const ble_addr_t kPeer = {0, {1, 2, 3, 4, 5, 6}};
constexpr uint16_t kHandle = 42;

struct Attempt {
    int rc;
    int disconnect_after_ms;  // -1 leaves the connection present indefinitely.
};

Attempt attempts[3];
unsigned terminate_calls;
bool connected;
int lookup_error;
TickType_t now;
TickType_t elapsed;
int disconnect_at;

void reset()
{
    for (auto &attempt : attempts) {
        attempt = {0, 250};
    }
    terminate_calls = 0;
    connected = true;
    lookup_error = 0;
    now = 0;
    elapsed = 0;
    disconnect_at = -1;
}

void test_absent_and_successful_disconnect()
{
    reset();
    connected = false;
    assert(ble_disconnect_peer(kPeer));
    assert(terminate_calls == 0);
    assert(elapsed == 0);

    reset();
    // There is deliberately no CONNECT or DISCONNECT callback in this harness.
    assert(ble_disconnect_peer(kPeer));
    assert(terminate_calls == 1);
    assert(elapsed == 250);
}

void test_request_failures_are_retried()
{
    reset();
    attempts[0] = {BLE_HS_EBUSY, -1};
    assert(ble_disconnect_peer(kPeer));
    assert(terminate_calls == 2);
    assert(elapsed == 500);

    reset();
    attempts[0] = attempts[1] = {BLE_HS_EBUSY, -1};
    assert(ble_disconnect_peer(kPeer));
    assert(terminate_calls == 3);
    assert(elapsed == 750);
}

void test_already_terminating_waits_for_confirmation()
{
    reset();
    attempts[0] = {BLE_HS_EALREADY, 750};
    assert(ble_disconnect_peer(kPeer));
    assert(terminate_calls == 1);
    assert(elapsed == 750);
}

void test_timeouts_and_async_failure_are_bounded()
{
    reset();
    // Models NimBLE retaining its terminating flag after TERM_FAILURE or a
    // missing completion: later requests return EALREADY with the link alive.
    attempts[0] = {0, -1};
    attempts[1] = attempts[2] = {BLE_HS_EALREADY, -1};
    assert(!ble_disconnect_peer(kPeer));
    assert(connected);
    assert(terminate_calls == 3);
    assert(elapsed == 15500);
}

void test_exhausted_requests_and_later_recovery()
{
    reset();
    for (auto &attempt : attempts) {
        attempt = {BLE_HS_EBUSY, -1};
    }
    assert(!ble_disconnect_peer(kPeer));
    assert(terminate_calls == 3);
    assert(elapsed == 500);

    // A later cleanup cycle can still release the retained connection.
    terminate_calls = 0;
    attempts[0] = {0, 250};
    assert(ble_disconnect_peer(kPeer));
    assert(terminate_calls == 1);
}

void test_disconnect_races_and_lookup_errors()
{
    reset();
    attempts[0] = {BLE_HS_ENOTCONN, 0};
    assert(ble_disconnect_peer(kPeer));
    assert(terminate_calls == 1);
    assert(elapsed == 0);

    reset();
    // Even an error on the final request may coincide with the peer leaving.
    attempts[0] = attempts[1] = {BLE_HS_EBUSY, -1};
    attempts[2] = {BLE_HS_EBUSY, 0};
    assert(ble_disconnect_peer(kPeer));
    assert(terminate_calls == 3);

    reset();
    lookup_error = BLE_HS_EDISABLED;
    assert(!ble_disconnect_peer(kPeer));
    assert(terminate_calls == 0);
    assert(elapsed == 500);
}

void test_tick_wrap()
{
    reset();
    now = UINT32_MAX - 100;
    attempts[0] = {BLE_HS_EALREADY, 1000};
    assert(ble_disconnect_peer(kPeer));
    assert(terminate_calls == 1);
    assert(elapsed == 1000);
}

}  // namespace

extern "C" int ble_gap_conn_find_by_addr(const ble_addr_t *addr, ble_gap_conn_desc *desc)
{
    assert(addr->type == kPeer.type && memcmp(addr->val, kPeer.val, sizeof(addr->val)) == 0);
    if (lookup_error != 0) {
        return lookup_error;
    }
    if (!connected) {
        return BLE_HS_ENOTCONN;
    }
    if (desc != nullptr) {
        desc->conn_handle = kHandle;
    }
    return 0;
}

extern "C" int ble_gap_terminate(uint16_t handle, uint8_t reason)
{
    assert(handle == kHandle);
    assert(reason == BLE_ERR_REM_USER_CONN_TERM);
    assert(terminate_calls < 3);
    const Attempt &attempt = attempts[terminate_calls++];
    if (attempt.disconnect_after_ms >= 0) {
        disconnect_at = static_cast<int>(elapsed) + attempt.disconnect_after_ms;
        if (attempt.disconnect_after_ms == 0) {
            connected = false;
        }
    }
    return attempt.rc;
}

TickType_t xTaskGetTickCount(void)
{
    return now;
}

void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0 && ticks <= 250);
    now += ticks;
    elapsed += ticks;
    if (disconnect_at >= 0 && elapsed >= static_cast<TickType_t>(disconnect_at)) {
        connected = false;
    }
}

int main()
{
    test_absent_and_successful_disconnect();
    test_request_failures_are_retried();
    test_already_terminating_waits_for_confirmation();
    test_timeouts_and_async_failure_are_bounded();
    test_exhausted_requests_and_later_recovery();
    test_disconnect_races_and_lookup_errors();
    test_tick_wrap();
    puts("BLE disconnect fault-injection tests: ok");
}
