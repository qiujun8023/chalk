#pragma once

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "utils/scoped_mutex_lock.h"

inline SemaphoreHandle_t GpioHoldMutex() {
    static StaticSemaphore_t s_mutex_buf;
    static SemaphoreHandle_t s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
    return s_mutex;
}

// 把"hold_dis → set_level → hold_en"三段式打包成一个 inline。
// 板上很多 rail 用 hold_en 锁电平避免 deep/light sleep 期间被 IO MUX 拉低,
// 改电平时必须先 hold_dis,否则 set_level 不生效;再 hold_en 重新锁回去。
//
// 多个 task 可能同时改 rail/PA pin；这里用全局短临界区避免三段式交错。
inline void GpioWriteHold(gpio_num_t pin, int level) {
    ScopedMutexLock lock(GpioHoldMutex());
    gpio_hold_dis(pin);
    gpio_set_level(pin, level);
    gpio_hold_en(pin);
}

inline void GpioWriteHold(int pin, int level) {
    GpioWriteHold(static_cast<gpio_num_t>(pin), level);
}
