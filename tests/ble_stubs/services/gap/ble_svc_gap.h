#pragma once

void ble_svc_gap_init(void);
int ble_svc_gap_device_name_set(const char *name);
const char *ble_svc_gap_device_name(void);
