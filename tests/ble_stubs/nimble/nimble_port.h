#pragma once

#include "esp_err.h"

struct ble_npl_event {
    void (*fn)(struct ble_npl_event *event);
    void *arg;
};
struct ble_npl_eventq { int unused; };
void ble_npl_event_init(struct ble_npl_event *event,
                        void (*fn)(struct ble_npl_event *), void *arg);
void ble_npl_eventq_put(struct ble_npl_eventq *queue, struct ble_npl_event *event);
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void);
esp_err_t nimble_port_init(void);
void nimble_port_run(void);
