#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class ScopedMutexLock {
   public:
    explicit ScopedMutexLock(SemaphoreHandle_t mutex) : mutex_(mutex) {
        configASSERT(mutex_);
        const bool ok = xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
        configASSERT(ok);
    }

    ~ScopedMutexLock() {
        if (mutex_)
            xSemaphoreGive(mutex_);
    }

    ScopedMutexLock(const ScopedMutexLock&)            = delete;
    ScopedMutexLock& operator=(const ScopedMutexLock&) = delete;

   private:
    SemaphoreHandle_t mutex_ = nullptr;
};
