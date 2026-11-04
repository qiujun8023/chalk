#pragma once

#include <esp_timer.h>

#include <cstdint>

namespace time_utils {

inline int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}

}  // namespace time_utils
