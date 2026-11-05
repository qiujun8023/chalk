#include "drivers/input/button.h"

#include <button_gpio.h>
#include <esp_log.h>

#include <utility>

namespace {
constexpr char kTag[] = "button_input";
}

// 不启用 iot_button 的 power_save：它注册的 GPIO 唤醒 ISR 不在 IRAM，flash cache 关闭期间（LittleFS 写入）触发会崩溃。
Button::Button(gpio_num_t gpio_num, bool active_high, uint16_t long_press_time, uint16_t short_press_time)
    : gpio_num_(gpio_num) {
    const button_config_t      button_config = {.long_press_time = long_press_time, .short_press_time = short_press_time};
    const button_gpio_config_t gpio_config   = {.gpio_num          = gpio_num,
                                                .active_level      = static_cast<uint8_t>(active_high ? 1 : 0),
                                                .enable_power_save = false,
                                                .disable_pull      = false};
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&button_config, &gpio_config, &button_handle_));
}

Button::~Button() {
    if (button_handle_ != nullptr)
        iot_button_delete(button_handle_);
}

void Button::Register(button_event_t event, std::function<void()>& storage, std::function<void()> callback) {
    storage = std::move(callback);
    const esp_err_t err = iot_button_register_cb(
        button_handle_, event, nullptr,
        [](void*, void* user) {
            auto* cb = static_cast<std::function<void()>*>(user);
            if (*cb)
                (*cb)();
        },
        &storage);
    if (err != ESP_OK)
        ESP_LOGW(kTag, "register failed gpio=%d event=%d err=%s", static_cast<int>(gpio_num_), static_cast<int>(event),
                 esp_err_to_name(err));
}

void Button::OnPressDown(std::function<void()> callback) {
    Register(BUTTON_PRESS_DOWN, on_press_down_, std::move(callback));
}

void Button::OnPressUp(std::function<void()> callback) {
    Register(BUTTON_PRESS_UP, on_press_up_, std::move(callback));
}

void Button::OnLongPress(std::function<void()> callback) {
    Register(BUTTON_LONG_PRESS_START, on_long_press_, std::move(callback));
}
