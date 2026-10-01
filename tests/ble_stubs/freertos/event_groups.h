#pragma once

#include "FreeRTOS.h"

typedef uint32_t EventBits_t;
typedef EventBits_t *EventGroupHandle_t;
EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                              BaseType_t clear, BaseType_t all, TickType_t timeout);
