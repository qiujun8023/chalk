#include "bsp/charge_status.h"

#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>

#include "bsp/config.h"
#include "utils/scoped_mutex_lock.h"
#include "utils/time_utils.h"

namespace {
constexpr char kTag[] = "charge";

bool SameSnapshot(const ChargeStatus::Snapshot& a, const ChargeStatus::Snapshot& b) {
    return a.state == b.state && a.power_present == b.power_present && a.charging == b.charging && a.full == b.full &&
           a.no_battery == b.no_battery;
}
}  // namespace

void ChargeStatus::Init(gpio_num_t detect_gpio, gpio_num_t full_gpio, int64_t now_ms) {
    detect_gpio_ = detect_gpio;
    full_gpio_   = full_gpio;
    if (!snapshot_mutex_) {
        snapshot_mutex_ = xSemaphoreCreateMutex();
        if (!snapshot_mutex_) {
            ESP_LOGE(kTag, "snapshot mutex create failed");
            configASSERT(snapshot_mutex_ != nullptr);
        }
    }
    if (!callback_mutex_) {
        callback_mutex_ = xSemaphoreCreateMutex();
        if (!callback_mutex_) {
            ESP_LOGE(kTag, "callback mutex create failed");
            configASSERT(callback_mutex_ != nullptr);
        }
    }
    if (!tick_exit_) {
        tick_exit_ = xSemaphoreCreateBinary();
        if (!tick_exit_) {
            ESP_LOGE(kTag, "tick exit semaphore create failed");
            configASSERT(tick_exit_ != nullptr);
        }
    }

    gpio_config_t cfg = {};
    cfg.intr_type     = GPIO_INTR_DISABLE;
    cfg.mode          = GPIO_MODE_INPUT;
    cfg.pin_bit_mask  = (1ULL << detect_gpio_) | (1ULL << full_gpio_);
    cfg.pull_up_en    = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&cfg));

    UpdateSnapshot(State::kNoPower, false, false);
    Tick(now_ms);
}

void ChargeStatus::StartTick() {
    if (tick_running_.exchange(true, std::memory_order_acq_rel))
        return;
    if (tick_exit_) {
        while (xSemaphoreTake(tick_exit_, 0) == pdTRUE) {
        }
    }
    BaseType_t ok =
        xTaskCreatePinnedToCore(&ChargeStatus::TickTaskEntry, "charge_tick", 3 * 1024, this, 1, &tick_task_, 0);
    if (ok != pdPASS) {
        tick_running_.store(false, std::memory_order_release);
        tick_task_ = nullptr;
        ESP_LOGE(kTag, "task create failed name=charge_tick");
    }
}

void ChargeStatus::StopTick() {
    if (!tick_running_.exchange(false, std::memory_order_acq_rel))
        return;
    if (tick_exit_) {
        xSemaphoreTake(tick_exit_, pdMS_TO_TICKS(1000));
    }
}

void ChargeStatus::OnStateChanged(std::function<void(const Snapshot&)> cb) {
    if (callback_mutex_)
        xSemaphoreTake(callback_mutex_, portMAX_DELAY);
    on_state_changed_ = cb;
    if (callback_mutex_)
        xSemaphoreGive(callback_mutex_);
}

void ChargeStatus::TickTaskEntry(void* arg) {
    static_cast<ChargeStatus*>(arg)->TickTaskLoop();
}

void ChargeStatus::TickTaskLoop() {
    // 有外部电源时 500 ms：去抖窗口(kStableHighMs=400/kAltWindowMs=1500)需要密集采样，
    // 且此时不省电(充电中暂停睡眠)。纯电池(无外部电源)时只需察觉「USB 插入」这一个 LOW
    // 沿，放慢到 2 s 让自动 light sleep 睡得更久；插入后下一拍即切回 500 ms 完成去抖。
    constexpr TickType_t kPollPowered = pdMS_TO_TICKS(500);
    constexpr TickType_t kPollBattery = pdMS_TO_TICKS(2000);
    while (tick_running_.load(std::memory_order_acquire)) {
        Tick(time_utils::NowMs());
        const TickType_t poll = Get().power_present ? kPollPowered : kPollBattery;
        vTaskDelay(poll);
    }
    tick_task_ = nullptr;
    if (tick_exit_)
        xSemaphoreGive(tick_exit_);
    vTaskDelete(nullptr);
}

void ChargeStatus::Tick(int64_t now_ms) {
    const bool charging_detected = gpio_get_level(detect_gpio_) == CHARGE_DETECT_CHARGING_LEVEL;
    const bool full_high         = gpio_get_level(full_gpio_) == 1;

    if (charging_detected) {
        last_power_present_ms_ = now_ms;
        last_detect_seen_ms_   = now_ms;
        if (detect_high_start_ms_ < 0) {
            detect_high_start_ms_ = now_ms;
        }
    } else {
        detect_high_start_ms_ = -1;
    }

    if (full_high) {
        last_power_present_ms_ = now_ms;
        last_full_seen_ms_     = now_ms;
        if (full_high_start_ms_ < 0) {
            full_high_start_ms_ = now_ms;
        }
    } else {
        full_high_start_ms_ = -1;
    }

    const bool power_present =
        (last_power_present_ms_ >= 0) && ((now_ms - last_power_present_ms_) <= kPowerPresentHoldMs);

    const bool detect_stable = (detect_high_start_ms_ >= 0) && ((now_ms - detect_high_start_ms_) >= kStableHighMs);
    const bool full_stable   = (full_high_start_ms_ >= 0) && ((now_ms - full_high_start_ms_) >= kStableHighMs);

    const bool alt_seen = power_present && (last_detect_seen_ms_ >= 0) && (last_full_seen_ms_ >= 0) &&
                          ((now_ms - last_detect_seen_ms_) <= kAltWindowMs) &&
                          ((now_ms - last_full_seen_ms_) <= kAltWindowMs);

    const bool no_battery = alt_seen && !detect_stable && !full_stable;

    // 状态优先级(从高到低):
    //   1. 没电源 → kNoPower
    //   2. 检测到无电池(两线 ~1Hz 交替) → kNoBattery
    //   3. CHRG_L 稳定低（明确「正在充电」信号） → kCharging
    //      ← 这一条必须比 kFull 优先,否则:充电 IC 的 STDBY 是 open-drain,
    //      没装外部上拉时浮空读 1 是常态,会被误判 full_stable=true → kFull,
    //      表现为充电中显示满电图标而不是 BOLT。
    //   4. STDBY 稳定高 → kFull
    //   5. 兜底:有电源但都不稳定 → kCharging
    State state = State::kNoPower;
    if (!power_present) {
        state = State::kNoPower;
    } else if (no_battery) {
        state = State::kNoBattery;
    } else if (detect_stable) {
        state = State::kCharging;
    } else if (full_stable) {
        state = State::kFull;
    } else {
        state = State::kCharging;
    }

    UpdateSnapshot(state, power_present, no_battery);
}

void ChargeStatus::UpdateSnapshot(State state, bool power_present, bool no_battery) {
    Snapshot next{
        .state         = state,
        .power_present = power_present,
        .charging      = state == State::kCharging || state == State::kNoBattery,
        .full          = state == State::kFull,
        .no_battery    = no_battery,
    };
    {
        ScopedMutexLock lock(snapshot_mutex_);
        if (SameSnapshot(snapshot_, next))
            return;
        snapshot_ = next;
    }

    std::function<void(const Snapshot&)> cb;
    if (callback_mutex_)
        xSemaphoreTake(callback_mutex_, portMAX_DELAY);
    cb = on_state_changed_;
    if (callback_mutex_)
        xSemaphoreGive(callback_mutex_);
    if (cb)
        cb(next);
}

ChargeStatus::Snapshot ChargeStatus::Get() const {
    ScopedMutexLock lock(snapshot_mutex_);
    return snapshot_;
}
