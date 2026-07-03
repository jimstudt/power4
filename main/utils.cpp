#include "utils.hpp"

ScopedLock::ScopedLock(SemaphoreHandle_t semaphore, TickType_t timeout)
    : semaphore_(semaphore)
{
    if (semaphore_ != nullptr) {
        locked_ = xSemaphoreTake(semaphore_, timeout) == pdTRUE;
    }
}

ScopedLock::~ScopedLock(void)
{
    if (locked_) {
        xSemaphoreGive(semaphore_);
    }
}

bool ScopedLock::locked(void) const
{
    return locked_;
}
