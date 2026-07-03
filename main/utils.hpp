#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class ScopedLock {
public:
    explicit ScopedLock(SemaphoreHandle_t semaphore, TickType_t timeout = portMAX_DELAY);
    ~ScopedLock(void);

    ScopedLock(const ScopedLock &) = delete;
    ScopedLock &operator=(const ScopedLock &) = delete;

    bool locked(void) const;

private:
    SemaphoreHandle_t semaphore_ = nullptr;
    bool locked_ = false;
};
