#pragma once

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <cstdint>
#include <functional>

class ChargeStatus {
   public:
    enum class State : uint8_t {
        kNoPower   = 0,
        kCharging  = 1,
        kFull      = 2,
        kNoBattery = 3,
    };

    struct Snapshot {
        State state;
        bool  power_present;
        bool  charging;  // UI/LED charging indicator (includes no-battery)
        bool  full;
        bool  no_battery;
    };

    void     Init(gpio_num_t detect_gpio, gpio_num_t full_gpio, int64_t now_ms);
    void     StartTick();
    void     StopTick();
    Snapshot Get() const;
    // 回调在 Tick() 调用方上下文同步执行;只做 evt::Post 这类轻量转发。
    void OnStateChanged(std::function<void(const Snapshot&)> cb);

   private:
    static void TickTaskEntry(void* arg);
    void        TickTaskLoop();
    void        Tick(int64_t now_ms);
    void        UpdateSnapshot(State state, bool power_present, bool no_battery);

    gpio_num_t detect_gpio_ = GPIO_NUM_NC;
    gpio_num_t full_gpio_   = GPIO_NUM_NC;

    int64_t detect_high_start_ms_  = -1;
    int64_t full_high_start_ms_    = -1;
    int64_t last_detect_seen_ms_   = -1;
    int64_t last_full_seen_ms_     = -1;
    int64_t last_power_present_ms_ = -1;

    mutable SemaphoreHandle_t            snapshot_mutex_ = nullptr;
    Snapshot                             snapshot_{State::kNoPower, false, false, false, false};
    std::function<void(const Snapshot&)> on_state_changed_;
    SemaphoreHandle_t                    callback_mutex_ = nullptr;
    std::atomic<bool>                    tick_running_{false};
    TaskHandle_t                         tick_task_ = nullptr;
    SemaphoreHandle_t                    tick_exit_ = nullptr;

    static constexpr int kPowerPresentHoldMs = 1000;
    static constexpr int kStableHighMs       = 400;
    static constexpr int kAltWindowMs        = 1500;
};
