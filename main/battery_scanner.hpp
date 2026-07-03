#pragma once

#include "esp_err.h"

esp_err_t battery_scanner_start(void);
void battery_scanner_set_verbose(bool enabled);
bool battery_scanner_verbose_enabled(void);
