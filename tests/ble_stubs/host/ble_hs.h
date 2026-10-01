#pragma once

#define BLE_HS_EALREADY 2
#define BLE_HS_ENOTCONN 7
#define BLE_HS_EBUSY 15
#define BLE_HS_EDISABLED 30
#define BLE_HS_EAPP 9

struct ble_hs_cfg {
    void (*reset_cb)(int reason);
    void (*sync_cb)(void);
};
extern struct ble_hs_cfg ble_hs_cfg;
int ble_hs_synced(void);
void ble_hs_sched_reset(int reason);
