#include "app/input.h"

#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_log.h>

#include "bsp/board.h"
#include "drivers/input/button.h"

namespace input {

namespace {
constexpr char kTag[] = "input";

QueueHandle_t s_queue = nullptr;

constexpr uint8_t kUpBit    = 1u << 0;
constexpr uint8_t kDownBit  = 1u << 1;
constexpr uint8_t kEnterBit = 1u << 2;
constexpr uint8_t kSideBits = kUpBit | kDownBit;

// 按键回调都在 iot_button 的定时器上下文里串行执行。
// held：当前按住的键；consumed：已被组合键吃掉、松手后不再触发单击的键。
// 上下同时按住时组合开始：期间确认也按下就是三键（立即触发设置）；否则上下都松开时触发组列表。
uint8_t s_held     = 0;
uint8_t s_consumed = 0;
bool    s_combo    = false;
bool    s_settings = false;

void Post(Ev ev) {
    Event e{ev, 0};
    if (xQueueSend(s_queue, &e, 0) != pdTRUE)
        ESP_LOGW(kTag, "queue full ev=%d", static_cast<int>(ev));
}

void PostButton(Ev ev) {
    ESP_LOGI(kTag, "button ev=%d", static_cast<int>(ev));
    Post(ev);
}

void PressDown(uint8_t bit) {
    s_consumed &= ~bit;
    s_held |= bit;
    if ((s_held & kSideBits) != kSideBits)
        return;
    s_combo = true;
    s_consumed |= kSideBits;
    if ((s_held & kEnterBit) && !s_settings) {
        s_settings = true;
        s_consumed |= kEnterBit;
        PostButton(Ev::kSettings);
    }
}

void PressUp(uint8_t bit) {
    s_held &= ~bit;
    if (s_combo && !(s_held & kSideBits)) {
        if (!s_settings)
            PostButton(Ev::kList);
        s_combo    = false;
        s_settings = false;
    }
}

void PostUnlessConsumed(uint8_t bit, Ev ev) {
    if (!(s_consumed & bit))
        PostButton(ev);
}

// 长按和单击效果相同：小孩按得久一点也照样响应。
void Bind(Button* btn, uint8_t bit, Ev click) {
    btn->OnPressDown([bit] { PressDown(bit); });
    btn->OnPressUp([bit] { PressUp(bit); });
    btn->OnClick([bit, click] { PostUnlessConsumed(bit, click); });
    btn->OnLongPress([bit, click] { PostUnlessConsumed(bit, click); });
}

// 调试输入：u/d/e 单击，l 上+下（组列表），m 三键（设置），s 深睡，i 状态，p 导出屏幕。
void ConsoleTask(void*) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(kTag, "console driver install failed");
        vTaskDelete(nullptr);
    }
    usb_serial_jtag_vfs_use_driver();
    uint8_t ch = 0;
    while (true) {
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) != 1)
            continue;
        switch (ch) {
            case 'u': Post(Ev::kUp); break;
            case 'd': Post(Ev::kDown); break;
            case 'e': Post(Ev::kEnter); break;
            case 'l': Post(Ev::kList); break;
            case 'm': Post(Ev::kSettings); break;
            case 's': Post(Ev::kSleep); break;
            case 'i': Post(Ev::kInfo); break;
            case 'p': Post(Ev::kDump); break;
            default: continue;
        }
        ESP_LOGI(kTag, "console key=%c", ch);
    }
}
}  // namespace

void Install(QueueHandle_t queue) {
    s_queue     = queue;
    Board& b    = Board::Get();
    Bind(b.up_btn(), kUpBit, Ev::kUp);
    Bind(b.down_btn(), kDownBit, Ev::kDown);
    Bind(b.enter_btn(), kEnterBit, Ev::kEnter);
    xTaskCreatePinnedToCore(ConsoleTask, "console", 3072, nullptr, 2, nullptr, 0);
}

bool IsButton(Ev ev) {
    return ev != Ev::kAudioDone && ev != Ev::kCharge && ev != Ev::kSleep && ev != Ev::kInfo && ev != Ev::kDump;
}

}  // namespace input
