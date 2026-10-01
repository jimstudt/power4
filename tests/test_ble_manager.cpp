#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ble_manager.hpp"
#include "relay_gatt.hpp"
#include "freertos/event_groups.h"

extern "C" {
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
struct ble_hs_cfg ble_hs_cfg = {};
}

namespace {

EventBits_t event_bits;
ble_npl_eventq host_queue;
ble_npl_event *queued_event;
ble_gap_event_fn *advertising_callback;
unsigned queued_resets;
unsigned scheduled_resets;
unsigned advertisements;
bool host_context;
bool host_synced;
bool controller_reset_pending;
int address_error;
bool request_during_sync;
void (*on_wait)();

void sync_host()
{
    host_context = true;
    host_synced = true;
    ble_hs_cfg.sync_cb();
    host_context = false;
}

void dispatch_request()
{
    assert(queued_event != nullptr);
    ble_npl_event *event = queued_event;
    queued_event = nullptr;
    host_context = true;
    event->fn(event);
    host_context = false;
}

void reset_host()
{
    assert(controller_reset_pending);
    controller_reset_pending = false;
    host_context = true;
    host_synced = false;

    // Mimic NimBLE's order: clear links (and call their callbacks) before the
    // reset callback. Advertising must not restart while the stack is down.
    const unsigned before = advertisements;
    ble_gap_event event = {};
    event.type = BLE_GAP_EVENT_DISCONNECT;
    event.disconnect.reason = BLE_HS_EAPP;
    advertising_callback(&event, nullptr);
    assert(advertisements == before);
    ble_hs_cfg.reset_cb(BLE_HS_EAPP);
    host_context = false;
}

void complete_recovery()
{
    dispatch_request();
    reset_host();
    sync_host();
}

void test_start_and_reset_completion()
{
    assert(ble_manager_reset(10) == ESP_ERR_INVALID_STATE);
    assert(ble_manager_start() == ESP_OK);
    assert(ble_manager_wait_until_synced(10) == ESP_ERR_TIMEOUT);
    sync_host();
    assert(ble_manager_is_synced());
    assert(advertisements == 1);

    on_wait = complete_recovery;
    assert(ble_manager_reset(30000) == ESP_OK);
    assert(queued_resets == 1 && scheduled_resets == 1);
    assert(ble_manager_is_synced());
    assert(advertisements == 2);
}

void test_timeout_and_old_sync_do_not_complete_recovery()
{
    // The completion bit from the previous recovery must not satisfy this one.
    assert(ble_manager_reset(10) == ESP_ERR_TIMEOUT);
    assert(!ble_manager_is_synced());
    assert(ble_manager_restart_advertising() == ESP_ERR_INVALID_STATE);
    assert(queued_resets == 2 && scheduled_resets == 1);

    // A sync before the queued reset has run belongs to the old stack state.
    sync_host();
    assert(!ble_manager_is_synced());
    assert(ble_manager_reset(10) == ESP_ERR_TIMEOUT);
    assert(queued_resets == 2);

    dispatch_request();
    sync_host();
    assert(ble_manager_reset(10) == ESP_ERR_TIMEOUT);
    assert(queued_resets == 2 && scheduled_resets == 2);

    reset_host();
    assert(ble_manager_reset(10) == ESP_ERR_TIMEOUT);
    // Join the pending reset, then deliver its actual synchronization.
    on_wait = sync_host;
    assert(ble_manager_reset(30000) == ESP_OK);
    assert(queued_resets == 2 && scheduled_resets == 2);
    assert(advertisements == 3);
}

void test_failed_sync_and_late_recovery()
{
    address_error = BLE_HS_EAPP;
    on_wait = complete_recovery;
    assert(ble_manager_reset(30000) == ESP_ERR_TIMEOUT);
    assert(!ble_manager_is_synced());
    assert(queued_resets == 3 && scheduled_resets == 3);
    assert(advertisements == 3);

    // After the caller's timeout, NimBLE can resynchronize on its own. The
    // scanner's normal sync gate then opens without scheduling another reset.
    address_error = 0;
    sync_host();
    assert(ble_manager_wait_until_synced(30000) == ESP_OK);
    assert(queued_resets == 3 && scheduled_resets == 3);
    assert(advertisements == 4);
}

void test_request_during_an_ordinary_sync()
{
    // Model the scanner requesting recovery while an ordinary sync callback
    // is already running. That callback must not erase the pending request.
    request_during_sync = true;
    sync_host();
    assert(!ble_manager_is_synced());
    assert(queued_resets == 4 && scheduled_resets == 3);
    assert(advertisements == 4);
    on_wait = complete_recovery;
    assert(ble_manager_reset(30000) == ESP_OK);
    assert(queued_resets == 4 && scheduled_resets == 4);
    assert(advertisements == 5);
}

}  // namespace

EventGroupHandle_t xEventGroupCreate(void) { return &event_bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{
    const EventBits_t before = *group;
    *group &= ~bits;
    return before;
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
    return *group |= bits;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                              BaseType_t clear, BaseType_t all, TickType_t timeout)
{
    assert(clear == pdFALSE && all == pdTRUE && timeout > 0);
    // Match FreeRTOS: a stale matching bit returns immediately without running
    // queued work, so the tests catch accidental reuse of a prior completion.
    if ((*group & bits) != bits && on_wait != nullptr) {
        auto action = on_wait;
        on_wait = nullptr;
        action();
    }
    return *group;
}

extern "C" {
int ble_hs_synced(void) { return host_synced; }
void ble_hs_sched_reset(int reason)
{
    assert(host_context);
    assert(reason == BLE_HS_EAPP);
    assert(!controller_reset_pending);
    controller_reset_pending = true;
    ++scheduled_resets;
}
void ble_npl_event_init(ble_npl_event *event, void (*fn)(ble_npl_event *), void *arg)
{
    event->fn = fn;
    event->arg = arg;
}
void ble_npl_eventq_put(ble_npl_eventq *queue, ble_npl_event *event)
{
    assert(queue == &host_queue && queued_event == nullptr);
    queued_event = event;
    ++queued_resets;
}
ble_npl_eventq *nimble_port_get_dflt_eventq(void) { return &host_queue; }
esp_err_t nimble_port_init(void) { return ESP_OK; }
void nimble_port_run(void) {}
void nimble_port_freertos_init(void (*)(void *)) {}
void nimble_port_freertos_deinit(void) {}
void ble_svc_gap_init(void) {}
void ble_svc_gatt_init(void) {}
int ble_svc_gap_device_name_set(const char *name) { assert(strcmp(name, "power4") == 0); return 0; }
const char *ble_svc_gap_device_name(void) { return "power4"; }
int ble_hs_util_ensure_addr(int)
{
    if (request_during_sync) {
        request_during_sync = false;
        assert(ble_manager_reset(10) == ESP_ERR_TIMEOUT);
    }
    return address_error;
}
int ble_hs_id_infer_auto(int, uint8_t *type) { *type = 0; return 0; }
int ble_gap_adv_set_fields(const ble_hs_adv_fields *) { return 0; }
int ble_gap_adv_rsp_set_fields(const ble_hs_adv_fields *) { return 0; }
int ble_gap_adv_start(uint8_t, const ble_addr_t *, int32_t,
                      const ble_gap_adv_params *, ble_gap_event_fn *cb, void *)
{
    assert(host_synced);
    advertising_callback = cb;
    ++advertisements;
    return 0;
}
}

esp_err_t relay_gatt_register(void) { return ESP_OK; }
esp_err_t config_gatt_register(void) { return ESP_OK; }
const ble_uuid128_t *relay_gatt_service_uuid(void) { static ble_uuid128_t uuid; return &uuid; }

int main()
{
    test_start_and_reset_completion();
    test_timeout_and_old_sync_do_not_complete_recovery();
    test_failed_sync_and_late_recovery();
    test_request_during_an_ordinary_sync();
    puts("BLE manager reset/recovery tests: ok");
}
