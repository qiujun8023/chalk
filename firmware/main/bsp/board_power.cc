#include "bsp/board_power.h"

#include <driver/gpio.h>

#include "bsp/config.h"
#include "utils/gpio_util.h"

namespace {
constexpr gpio_num_t kLedPin = GPIO_NUM_3;  // 板上唯一一颗绿色 LED，低有效
}  // namespace

BoardPowerBsp::BoardPowerBsp(int audioPowerPin, int audioAmpPin, int vbatPowerPin)
    : audioPowerPin_(audioPowerPin), audioAmpPin_(audioAmpPin), vbatPowerPin_(vbatPowerPin) {
    // VBAT_PWR(GPIO17): 系统软锁存,拉高=自锁,拉低=断电(关机唯一手段)
    // Audio_PWR(GPIO42): AVDD_3V3 rail,关掉 = I²C 死(R45/R46 上拉在这条 rail)
    // Audio_AMP(GPIO46): PA U5 数字使能 + ES8311 PA_PIN。
    //
    // PA pin 必须跟 audio rail 在同一 gpio_config 里 init —— gpio_config 一返回
    // mode=OUTPUT level=0 立即生效,PA CTRL 稳是 LOW。后面 PowerAudioOn 拉高
    // GPIO42 给 PA U5 通电,CTRL 已经稳定 LOW,PA 不会放大 ES8311 默认 DC bias。
    // 这是消除开机"啵"声的关键时序点。AudioPlayer 后续负责在 codec dev open
    // + DAC 稳定 100 ms 后再拉高 PA（出声）。
    gpio_config_t cfg = {};
    cfg.intr_type     = GPIO_INTR_DISABLE;
    cfg.mode          = GPIO_MODE_OUTPUT;
    cfg.pin_bit_mask  = (1ULL << audioPowerPin_) | (1ULL << audioAmpPin_) | (1ULL << vbatPowerPin_);
    cfg.pull_up_en    = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&cfg));
    // 显式拉低 + hold_en:gpio_config 默认 level=0 但稳起见写一遍,且 hold 让
    // 电平在 deep sleep / 复位过程中也不丢(不然醒来到 audio init 之间又是浮空)。
    gpio_set_level(static_cast<gpio_num_t>(audioAmpPin_), 0);
    gpio_hold_en(static_cast<gpio_num_t>(audioAmpPin_));
}

void BoardPowerBsp::InitLed() {
    // GPIO3 是 strapping pin（高=不打 ROM log）。板上 R35 已经把它上拉到 3V3，
    // 所以复位瞬间 LED 灭、ROM 不打 log。这里再配置成 OUTPUT 并写高（继续灭）。
    gpio_config_t cfg = {};
    cfg.intr_type     = GPIO_INTR_DISABLE;
    cfg.mode          = GPIO_MODE_OUTPUT;
    cfg.pin_bit_mask  = 1ULL << kLedPin;
    cfg.pull_up_en    = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&cfg));
    GpioWriteHold(kLedPin, 1);
}

void BoardPowerBsp::PowerAudioOn() {
    GpioWriteHold(audioPowerPin_, 1);
}
void BoardPowerBsp::PowerAudioOff() {
    GpioWriteHold(audioPowerPin_, 0);
}
void BoardPowerBsp::VbatPowerOn() {
    GpioWriteHold(vbatPowerPin_, 1);
}
void BoardPowerBsp::VbatPowerOff() {
    GpioWriteHold(vbatPowerPin_, 0);
}

extern "C" void BoardI2cForcePowerOn() {
    GpioWriteHold(AUDIO_PWR_PIN, AUDIO_PWR_FORCE_LEVEL);
}
