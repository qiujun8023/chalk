#include "drivers/input/button.h"

#include <button_gpio.h>
#include <esp_log.h>

#include <utility>

namespace {
constexpr char kTag[] = "button_input";
}

Button::Button(button_handle_t button_handle) : button_handle_(button_handle) {
    ESP_LOGD(kTag, "wrap handle=%p", button_handle_);
}

Button::Button(gpio_num_t gpio_num, bool active_high, uint16_t long_press_time, uint16_t short_press_time,
               bool enable_power_save)
    : gpio_num_(gpio_num) {
    if (gpio_num == GPIO_NUM_NC) {
        ESP_LOGD(kTag, "create skip gpio=nc");
        return;
    }
    ESP_LOGD(kTag, "create gpio=%d active_high=%d long_ms=%u short_ms=%u power_save=%d", static_cast<int>(gpio_num),
             active_high ? 1 : 0, static_cast<unsigned>(long_press_time), static_cast<unsigned>(short_press_time),
             enable_power_save ? 1 : 0);
    button_config_t      button_config = {.long_press_time = long_press_time, .short_press_time = short_press_time};
    button_gpio_config_t gpio_config   = {.gpio_num          = gpio_num,
                                          .active_level      = static_cast<uint8_t>(active_high ? 1 : 0),
                                          .enable_power_save = enable_power_save,
                                          .disable_pull      = false};
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&button_config, &gpio_config, &button_handle_));
    ESP_LOGD(kTag, "created gpio=%d handle=%p", static_cast<int>(gpio_num_), button_handle_);
}

Button::~Button() {
    if (button_handle_ != nullptr) {
        ESP_LOGD(kTag, "delete gpio=%d handle=%p", static_cast<int>(gpio_num_), button_handle_);
        iot_button_delete(button_handle_);
    }
}

bool Button::RegisterCallback(button_event_t event, button_event_args_t* args, button_cb_t cb, void* user_data,
                              bool& registered) {
    if (button_handle_ == nullptr) {
        return false;
    }
    if (registered) {
        esp_err_t err = iot_button_unregister_cb(button_handle_, event, args);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "unregister failed event=%d err=%s", static_cast<int>(event), esp_err_to_name(err));
            return false;
        }
        registered = false;
    }
    esp_err_t err = iot_button_register_cb(button_handle_, event, args, cb, user_data);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "register failed event=%d err=%s", static_cast<int>(event), esp_err_to_name(err));
        return false;
    }
    registered = true;
    ESP_LOGD(kTag, "registered gpio=%d event=%d user=%p", static_cast<int>(gpio_num_), static_cast<int>(event),
             user_data);
    return true;
}

void Button::RegisterSimpleEvent(button_event_t event, std::function<void()>& storage, bool& registered,
                                 std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    storage = std::move(callback);
    RegisterCallback(
        event, nullptr,
        [](void* handle, void* usr_data) {
            auto* cb = static_cast<std::function<void()>*>(usr_data);
            ESP_LOGD(kTag, "callback handle=%p user=%p", handle, usr_data);
            if (*cb)
                (*cb)();
        },
        &storage, registered);
}

void Button::OnPressDown(std::function<void()> callback) {
    RegisterSimpleEvent(BUTTON_PRESS_DOWN, on_press_down_, press_down_registered_, std::move(callback));
}

void Button::OnPressUp(std::function<void()> callback) {
    RegisterSimpleEvent(BUTTON_PRESS_UP, on_press_up_, press_up_registered_, std::move(callback));
}

void Button::OnLongPress(std::function<void()> callback) {
    RegisterSimpleEvent(BUTTON_LONG_PRESS_START, on_long_press_, long_press_registered_, std::move(callback));
}

void Button::OnClick(std::function<void()> callback) {
    RegisterSimpleEvent(BUTTON_SINGLE_CLICK, on_click_, click_registered_, std::move(callback));
}
