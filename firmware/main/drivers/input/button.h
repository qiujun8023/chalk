#pragma once

#include <iot_button.h>

#include <driver/gpio.h>

#include <functional>

// iot_button 的薄封装：一个 GPIO 按键，回调在 iot_button 的定时器上下文里执行。
class Button {
   public:
    Button(gpio_num_t gpio_num, bool active_high = false, uint16_t long_press_time = 0, uint16_t short_press_time = 0);
    ~Button();

    Button(const Button&)            = delete;
    Button& operator=(const Button&) = delete;

    void OnPressDown(std::function<void()> callback);
    void OnPressUp(std::function<void()> callback);
    void OnLongPress(std::function<void()> callback);  // 按住到 long_press_time 时触发一次

   private:
    // 每个事件只注册一次；回调存在成员里，地址作为 iot_button 的 user_data。
    void Register(button_event_t event, std::function<void()>& storage, std::function<void()> callback);

    gpio_num_t      gpio_num_      = GPIO_NUM_NC;
    button_handle_t button_handle_ = nullptr;

    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_long_press_;
};
