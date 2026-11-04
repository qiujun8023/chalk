#pragma once

#include <button_gpio.h>
#include <button_types.h>
#include <driver/gpio.h>
#include <iot_button.h>
#include <functional>

class Button {
   public:
    Button(button_handle_t button_handle);
    Button(gpio_num_t gpio_num, bool active_high = false, uint16_t long_press_time = 0, uint16_t short_press_time = 0,
           bool enable_power_save = false);
    ~Button();

    void OnPressDown(std::function<void()> callback);
    void OnPressUp(std::function<void()> callback);
    void OnLongPress(std::function<void()> callback);
    void OnClick(std::function<void()> callback);

   protected:
    gpio_num_t      gpio_num_      = GPIO_NUM_NC;
    button_handle_t button_handle_ = nullptr;

    bool RegisterCallback(button_event_t event, button_event_args_t* args, button_cb_t cb, void* user_data,
                          bool& registered);
    void RegisterSimpleEvent(button_event_t event, std::function<void()>& storage, bool& registered,
                             std::function<void()> callback);

    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_long_press_;
    std::function<void()> on_click_;

    bool press_down_registered_ = false;
    bool press_up_registered_   = false;
    bool long_press_registered_ = false;
    bool click_registered_      = false;
};
