#include "drivers/display/epd_ssd1683.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "drivers/display/framebuffer_ops.h"
#include "utils/time_utils.h"

void EpdSsd1683::EpdInit() {
    EpdPowerOn();
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(rst_, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(rst_, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(rst_, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    ReadBusy();
    EpdSendCommand(0x00);
    EpdSendData(0x2F);
    EpdSendData(0x2E);
    EpdSendCommand(0xE9);
    EpdSendData(0x01);
    ReadBusy();
}

void EpdSsd1683::ApplyTemperatureBoost() {
    // 0x40 = Get Temp,ReadBusy 后 EPD 把片内温度寄存器值放到 DI 上,SPI 反向读 1B。
    // 这次切换 SPI 模式约 5~10 ms，所以 60 s 内复用上次结果（屏温变化很慢）。
    constexpr int64_t kCacheValidMs = 60 * 1000;
    const int64_t     now_ms        = time_utils::NowMs();

    uint8_t booster;
    if (cached_booster_ != 0 && (now_ms - last_temp_read_ms_) < kCacheValidMs) {
        booster = cached_booster_;
    } else {
        EpdSendCommand(0x40);
        ReadBusy();
        const uint8_t temp = EpdRecvData();
        // 5 档:≤5°C 用 -24°C 偏置(0xE8),≤10 用 -21,≤20 用 -18,≤30 用 -15,
        // ≤127 用 -12;>127(寄存器异常)按最冷处理。
        if (temp <= 5)
            booster = 232;
        else if (temp <= 10)
            booster = 235;
        else if (temp <= 20)
            booster = 238;
        else if (temp <= 30)
            booster = 241;
        else if (temp <= 127)
            booster = 244;
        else
            booster = 232;
        cached_booster_    = booster;
        last_temp_read_ms_ = now_ms;
    }

    EpdSendCommand(0xE0);
    EpdSendData(0x02);
    EpdSendCommand(0xE6);
    EpdSendData(booster);
}

void EpdSsd1683::EpdDisplayFull() {
    int      bpr     = (kWidth + 7) >> 3;
    int      bpr_out = bpr * 2;
    uint8_t* line    = epd_line_.data();

    ApplyTemperatureBoost();
    EpdSendCommand(0xA5);  // Master Activation:加载 LUT(full 模式必需)
    ReadBusy();
    vTaskDelay(pdMS_TO_TICKS(10));

    EpdSendCommand(0x10);
    for (int y = 0; y < kHeight; ++y) {
        const uint8_t* src = snapshot_ + y * bpr;
        uint8_t*       dst = line;
        for (int xb = 0; xb < bpr; ++xb) {
            uint8_t a, b;
            epd::Pack1bppTo2683(src[xb], a, b);
            *dst++ = a;
            *dst++ = b;
        }
        WriteBytes(line, bpr_out);
    }
    EpdTurnOnDisplay();
}

void EpdSsd1683::EpdDisplayPartial() {
    int      bpr     = (kWidth + 7) >> 3;
    int      bpr_out = bpr * 2;
    uint8_t* line    = epd_line_.data();

    // 不要重写 booster!
    // 之前这里调了 ApplyTemperatureBoost 想"低温补偿",但实测每次 partial 前
    // 发 0xE0 0xE6 会把 SSD1683 切回 boost-charge 状态,partial LUT 失效,
    // 屏幕看起来"日志在刷但完全没变化"。booster 在上一轮 FULL 路径的
    // ApplyTemperatureBoost + EpdInit 时已经设好,partial 复用即可。
    // 参考 esp32-eink/.../custom_lcd_display.cc:1135 EPD_DisplayPart 同样不写 booster。

    EpdSendCommand(0x10);
    ReadBusy();  // 跟参考实现对齐:0x10 之后等 BUSY 回 HIGH
    for (int y = 0; y < kHeight; ++y) {
        const uint8_t* prev = prev_snapshot_ + y * bpr;
        const uint8_t* now  = snapshot_ + y * bpr;
        for (int xb = 0; xb < bpr; ++xb) {
            epd::PackPartial1bppTo2683(prev[xb], now[xb], line[2 * xb + 0], line[2 * xb + 1]);
        }
        WriteBytes(line, bpr_out);
    }
    EpdTurnOnDisplay();
}

void EpdSsd1683::EpdTurnOnDisplay() {
    EpdSendCommand(0x04);  // power on
    ReadBusy();
    EpdSendCommand(0x12);  // display refresh
    EpdSendData(0x00);
    ReadBusy();
    EpdSendCommand(0x02);  // power off (controller internal)
    EpdSendData(0x00);
    ReadBusy();
    // 跟参考实现对齐:每次刷完屏都断 GPIO6,跟刷新前的 EpdInit() 内 EpdPowerOn 配对。
    // 见 esp32-eink/main/boards/zectrix-s3-epaper-4.2/custom_lcd_display.cc:826。
    EpdPowerOff();
}
