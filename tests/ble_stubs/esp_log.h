#pragma once

// Keep format checking enabled without printing expected fault-injection logs.
#include <stdio.h>
#define ESP_LOGW(tag, ...) do { (void)(tag); if (false) { printf(__VA_ARGS__); } } while (0)
#define ESP_LOGI ESP_LOGW
#define ESP_LOGE ESP_LOGW
