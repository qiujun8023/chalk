#include "bsp/board.h"

#include <driver/rtc_io.h>
#include <esp_log.h>
#include <esp_sleep.h>

#include "bsp/battery_adc.h"
#include "bsp/board_power.h"
#include "bsp/charge_status.h"
#include "bsp/config.h"
#include "drivers/bus/i2c_bus_lock.h"
#include "drivers/display/epd_ssd1683.h"
#include "drivers/input/button.h"
#include "utils/time_utils.h"

namespace {
constexpr char     kTag[]       = "board";
constexpr uint16_t kLongPressMs = 1000;

// 深睡前用 rtc_gpio_hold_en 锁住的唤醒脚，醒来后要交还数字 IO 矩阵，按键和充电检测才能工作。
// VBAT_PWR(GPIO17) 不在这里释放：它是供电自锁，由 VbatPowerOn 先驱动高再切域。
void ReleaseDeepSleepWakeHolds() {
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED)
        return;
    for (gpio_num_t pin : {BOOT_BUTTON_GPIO, DOWN_BUTTON_GPIO, CHARGE_DETECT_GPIO}) {
        rtc_gpio_hold_dis(pin);
        rtc_gpio_deinit(pin);
    }
}
}  // namespace

Board& Board::Get() {
    static Board s;
    return s;
}

void Board::Init() {
    ReleaseDeepSleepWakeHolds();
    InitPower();
    InitI2c();
    charge_->Init(CHARGE_DETECT_GPIO, CHARGE_FULL_GPIO, time_utils::NowMs());
    charge_->StartTick();
    power_->InitLed();
    epd_ = std::make_unique<EpdSsd1683>();
    epd_->Init();
    InitButtons();
    battery_ = std::make_unique<BatteryAdc>();
    battery_->Init();
}

bool Board::ReadBattery(uint16_t* voltage_mv, uint8_t* percent) {
    if (!battery_ || charge_->Get().no_battery)
        return false;
    return battery_->Read(voltage_mv, percent);
}

void Board::InitPower() {
    charge_ = std::make_unique<ChargeStatus>();
    // 构造时一次配置 audio rail / PA / VBAT，PA 先稳定为 LOW 再给 audio rail 上电，避免开机爆音。
    power_ = std::make_unique<BoardPowerBsp>(AUDIO_PWR_PIN, AUDIO_CODEC_PA_PIN, VBAT_PWR_PIN);
    power_->VbatPowerOn();
    power_->PowerAudioOn();

    // 开机时下键按住接通电源；等它松开再初始化按键，否则会被识别成一次按键。
    constexpr int kMaxWaitMs = 2000;
    int           waited     = 0;
    while (!gpio_get_level(POWER_KEY_GPIO) && waited < kMaxWaitMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    ESP_LOGD(kTag, "power key released wait_ms=%d", waited);
}

void Board::InitI2c() {
    ScopedI2cBusLock lock("Board::InitI2c");
    ESP_ERROR_CHECK(lock.status());
    i2c_master_bus_config_t cfg      = {};
    cfg.i2c_port                     = I2C_NUM_0;
    cfg.sda_io_num                   = AUDIO_CODEC_I2C_SDA_PIN;
    cfg.scl_io_num                   = AUDIO_CODEC_I2C_SCL_PIN;
    cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt            = 7;
    cfg.flags.enable_internal_pullup = 1;
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus_));
}

void Board::InitButtons() {
    // 不启用 iot_button 的 power_save：它注册的 GPIO 唤醒 ISR 不在 IRAM，
    // flash cache 关闭期间（LittleFS 写入）触发会崩溃。深睡唤醒由 power/sleep 单独配置。
    up_btn_    = std::make_unique<Button>(UP_BUTTON_GPIO, false, kLongPressMs, 0);
    down_btn_  = std::make_unique<Button>(DOWN_BUTTON_GPIO, false, kLongPressMs, 0);
    enter_btn_ = std::make_unique<Button>(BOOT_BUTTON_GPIO, false, kLongPressMs, 0);
}
