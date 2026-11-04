#include "drivers/display/epd_ssd1683.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "bsp/config.h"
#include "utils/time_utils.h"

namespace {
constexpr char kTag[] = "epd";

// 两次刷新之间至少间隔这么久，防止过频刷新损伤面板。
constexpr int64_t kMinRefreshGapMs = 300;
}  // namespace

EpdSsd1683::EpdSsd1683() {
    cs_       = EPD_CS_PIN;
    dc_       = EPD_DC_PIN;
    rst_      = EPD_RST_PIN;
    busy_     = EPD_BUSY_PIN;
    mosi_     = EPD_MOSI_PIN;
    sclk_     = EPD_SCK_PIN;
    spi_host_ = EPD_SPI_NUM;
}

void EpdSsd1683::Init() {
    SpiPortInit();
    SpiGpioInit();
    // 初始化阶段不刷屏：面板断电后保留画面，由上层决定首帧。
    EpdPowerOff();

    snapshot_      = static_cast<uint8_t*>(heap_caps_malloc(kBufferLen, MALLOC_CAP_SPIRAM));
    prev_snapshot_ = static_cast<uint8_t*>(heap_caps_malloc(kBufferLen, MALLOC_CAP_SPIRAM));
    if (!snapshot_ || !prev_snapshot_) {
        ESP_LOGE(kTag, "framebuffer alloc failed action=restart");
        esp_restart();
    }
    memset(snapshot_, 0xFF, kBufferLen);
    memset(prev_snapshot_, 0xFF, kBufferLen);
    epd_line_.resize(kWidth / 8 * 2);
}

void EpdSsd1683::Seed(const uint8_t* frame) {
    memcpy(prev_snapshot_, frame, kBufferLen);
    synced_ = true;
}

EpdSsd1683::Mode EpdSsd1683::Show(const uint8_t* frame, Mode mode) {
    if (mode == Mode::kPartial && (!synced_ || partial_since_full_ >= kMaxPartialBeforeFull))
        mode = Mode::kFull;

    const int64_t gap = time_utils::NowMs() - last_refresh_end_ms_;
    if (gap < kMinRefreshGapMs)
        vTaskDelay(pdMS_TO_TICKS(kMinRefreshGapMs - gap));

    memcpy(snapshot_, frame, kBufferLen);
    const int64_t start_ms = time_utils::NowMs();
    // 每次刷新都先硬复位：否则上一轮全刷留下的 LUT 会让局刷变成闪烁的全刷。
    EpdInit();
    if (mode == Mode::kFull) {
        EpdDisplayFull();
        partial_since_full_ = 0;
    } else {
        EpdDisplayPartial();
        partial_since_full_++;
    }
    memcpy(prev_snapshot_, snapshot_, kBufferLen);
    synced_              = true;
    last_refresh_end_ms_ = time_utils::NowMs();
    ESP_LOGI(kTag, "refresh mode=%s elapsed_ms=%lld partial_since_full=%d", mode == Mode::kFull ? "full" : "partial",
             static_cast<long long>(last_refresh_end_ms_ - start_ms), partial_since_full_);
    return mode;
}
