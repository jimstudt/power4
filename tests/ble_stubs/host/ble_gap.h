#pragma once

#include <stdint.h>
#include "ble_uuid.h"

typedef struct {
    uint8_t type;
    uint8_t val[6];
} ble_addr_t;

struct ble_gap_conn_desc {
    uint16_t conn_handle;
};

#define BLE_ERR_REM_USER_CONN_TERM 0x13

int ble_gap_conn_find_by_addr(const ble_addr_t *addr, struct ble_gap_conn_desc *desc);
int ble_gap_terminate(uint16_t conn_handle, uint8_t reason);

#define BLE_GAP_EVENT_CONNECT 0
#define BLE_GAP_EVENT_DISCONNECT 1
#define BLE_GAP_EVENT_ADV_COMPLETE 9
#define BLE_HS_ADV_F_DISC_GEN 0x02
#define BLE_HS_ADV_F_BREDR_UNSUP 0x04
#define BLE_HS_ADV_TX_PWR_LVL_AUTO 127
#define BLE_GAP_CONN_MODE_UND 2
#define BLE_GAP_DISC_MODE_GEN 2
#define BLE_HS_FOREVER INT32_MAX

struct ble_gap_event {
    uint8_t type;
    struct { int status; } connect;
    struct { int reason; } disconnect;
    struct { int reason; } adv_complete;
};

struct ble_hs_adv_fields {
    uint8_t flags;
    uint8_t tx_pwr_lvl_is_present;
    int8_t tx_pwr_lvl;
    const ble_uuid128_t *uuids128;
    uint8_t num_uuids128;
    uint8_t uuids128_is_complete;
    const uint8_t *name;
    uint8_t name_len;
    uint8_t name_is_complete;
};

struct ble_gap_adv_params {
    uint8_t conn_mode;
    uint8_t disc_mode;
};

typedef int ble_gap_event_fn(struct ble_gap_event *event, void *arg);
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *fields);
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *fields);
int ble_gap_adv_start(uint8_t addr_type, const ble_addr_t *peer, int32_t duration,
                      const struct ble_gap_adv_params *params, ble_gap_event_fn *cb, void *arg);
