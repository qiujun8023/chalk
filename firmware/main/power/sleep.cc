#include "power/sleep.h"

#include <driver/rtc_io.h>
#include <esp_log.h>
#include <esp_sleep.h>

#include "bsp/config.h"
#include "utils/gpio_util.h"

namespace power {

namespace {
constexpr char kTag[] = "sleep";

// EXT1 唤醒脚必须切到 RTC 数字输入 + 上拉 + hold；iot_button 之前占用过 IO MUX，
// 只调 rtc_gpio_init 不改 direction 时 EXT1 感知不到电平。
void PrepareWakeupGpio(gpio_num_t pin) {
    rtc_gpio_init(pin);
    rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(pin);
    rtc_gpio_pullup_en(pin);
    rtc_gpio_hold_en(pin);
}

// VBAT_PWR(GPIO17) 拉低即整机断电，按键也唤不醒。GPIO_RESET_WORKAROUND 会在深睡期间复位普通 GPIO，
// 必须切到 RTC 域并 hold 高电平。
void LockVbatPowerHigh() {
    gpio_hold_dis(VBAT_PWR_PIN);
    rtc_gpio_init(VBAT_PWR_PIN);
    rtc_gpio_set_direction(VBAT_PWR_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_pulldown_dis(VBAT_PWR_PIN);
    rtc_gpio_pullup_dis(VBAT_PWR_PIN);
    rtc_gpio_set_level(VBAT_PWR_PIN, 1);
    rtc_gpio_hold_en(VBAT_PWR_PIN);
}
}  // namespace

WakeSource DetectWakeSource() {
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_UNDEFINED)
        return WakeSource::kColdBoot;
    if (cause != ESP_SLEEP_WAKEUP_EXT1)
        return WakeSource::kOther;
    const uint64_t mask = esp_sleep_get_ext1_wakeup_status();
    if (mask & (1ULL << BOOT_BUTTON_GPIO))
        return WakeSource::kEnter;
    if (mask & (1ULL << DOWN_BUTTON_GPIO))
        return WakeSource::kDown;
    if (mask & (1ULL << CHARGE_DETECT_GPIO))
        return WakeSource::kCharge;
    return WakeSource::kOther;
}

void EnterDeepSleep() {
    ESP_LOGI(kTag, "deep sleep enter");
    GpioWriteHold(AUDIO_CODEC_PA_PIN, 0);
    GpioWriteHold(EPD_PWR_PIN, 0);
    // audio rail 关闭后 I²C 上拉失效，必须放在所有 I²C 操作之后。
    GpioWriteHold(AUDIO_PWR_PIN, 0);
    LockVbatPowerHigh();

    // 上键 GPIO39 不是 RTC IO，不能唤醒。
    PrepareWakeupGpio(BOOT_BUTTON_GPIO);
    PrepareWakeupGpio(DOWN_BUTTON_GPIO);
    PrepareWakeupGpio(CHARGE_DETECT_GPIO);
    constexpr uint64_t kMask = (1ULL << BOOT_BUTTON_GPIO) | (1ULL << DOWN_BUTTON_GPIO) | (1ULL << CHARGE_DETECT_GPIO);
    esp_sleep_enable_ext1_wakeup(kMask, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

}  // namespace power
