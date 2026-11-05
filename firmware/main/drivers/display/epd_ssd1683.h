#pragma once

#include <driver/gpio.h>
#include <driver/spi_master.h>

#include <cstdint>
#include <vector>

// 4.2" 黑白 EPD（400×300，1bpp，SSD2683 兼容 SSD1683 命令）的同步驱动。
// 帧格式：每行 50 字节，MSB 在左，bit=1 白、bit=0 黑。
// Show 阻塞到刷新完成，只能由一个 task 调用。
class EpdSsd1683 {
   public:
    static constexpr int kWidth     = 400;
    static constexpr int kHeight    = 300;
    static constexpr int kBufferLen = kWidth / 8 * kHeight;

    // 连续局刷达到这个次数后强制全刷，防止残影累积。上层通常会更早安排全刷。
    static constexpr int kMaxPartialBeforeFull = 16;

    enum class Mode { kFull, kPartial };

    EpdSsd1683();

    void Init();

    // 刷新一帧。没有已知的上一帧或局刷次数超限时自动改为全刷。返回实际使用的模式。
    Mode Show(const uint8_t* frame, Mode mode);

    // 深睡唤醒后屏幕仍保留画面，用它告诉驱动当前画面，下一次就能直接局刷。
    void Seed(const uint8_t* frame);

    // 屏幕上当前的画面（最近一次刷新的帧）。
    const uint8_t* current() const {
        return prev_snapshot_;
    }

    int partial_since_full() const {
        return partial_since_full_;
    }

   private:
    spi_device_handle_t spi_        = nullptr;
    bool                spi_inited_ = false;

    uint8_t*             snapshot_      = nullptr;  // 本次要刷的帧
    uint8_t*             prev_snapshot_ = nullptr;  // 屏幕上当前的帧
    bool                 synced_        = false;
    int                  partial_since_full_ = 0;
    std::vector<uint8_t> epd_line_;

    int64_t last_temp_read_ms_ = 0;
    uint8_t cached_booster_    = 0;

    void    SpiPortInit(bool rx);
    void    SpiGpioInit();
    void    EpdInit();
    void    EpdDisplayFull();
    void    EpdDisplayPartial();
    void    EpdTurnOnDisplay();
    void    ApplyTemperatureBoost();
    void    EpdSendCommand(uint8_t c);
    void    EpdSendData(uint8_t d);
    uint8_t EpdRecvData();
    void    WriteBytes(const uint8_t* buf, int len);
    void    ReadBusy();
    void    EpdPowerOn();
    void    EpdPowerOff();

    gpio_num_t        cs_, dc_, rst_, busy_, mosi_, sclk_;
    spi_host_device_t spi_host_;
};
