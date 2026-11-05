#include "drivers/display/epd_ssd1683.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bsp/config.h"
#include "utils/gpio_util.h"
#include "utils/time_utils.h"

namespace {
constexpr char kTag[] = "epd";
}

// EPD 只有一根数据线 DI：写命令 / 数据时它是 MOSI，读温度时同一引脚反向当 MISO，所以读写之间要拆掉 SPI 总线重建。
void EpdSsd1683::SpiPortInit(bool rx) {
    if (spi_inited_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_remove_device(spi_));
        spi_ = nullptr;
        const esp_err_t err = spi_bus_free(spi_host_);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            ESP_ERROR_CHECK(err);
        spi_inited_ = false;
    }
    spi_bus_config_t b              = {};
    b.miso_io_num                   = rx ? mosi_ : -1;
    b.mosi_io_num                   = rx ? -1 : mosi_;
    b.sclk_io_num                   = sclk_;
    b.quadwp_io_num                 = -1;
    b.quadhd_io_num                 = -1;
    b.max_transfer_sz               = kBufferLen * 2;
    spi_device_interface_config_t d = {};
    d.spics_io_num                  = -1;
    d.clock_speed_hz                = (rx ? 8 : 40) * 1000 * 1000;  // 读时降速到 8 MHz
    d.mode                          = 0;
    d.queue_size                    = 7;
    ESP_ERROR_CHECK(spi_bus_initialize(spi_host_, &b, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(spi_host_, &d, &spi_));
    spi_inited_ = true;
}

uint8_t EpdSsd1683::EpdRecvData() {
    SpiPortInit(/*rx=*/true);
    uint8_t           rx = 0;
    spi_transaction_t t  = {};
    t.length             = 8;
    t.rx_buffer          = &rx;
    gpio_set_level(dc_, 1);
    gpio_set_level(cs_, 0);
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(cs_, 1);
    SpiPortInit(/*rx=*/false);
    return rx;
}

void EpdSsd1683::SpiGpioInit() {
    // EPD_PWR_PIN(GPIO6) 由本类自管(BoardPowerBsp 不再接管),先配成 OUTPUT。
    gpio_config_t gpwr = {};
    gpwr.intr_type     = GPIO_INTR_DISABLE;
    gpwr.mode          = GPIO_MODE_OUTPUT;
    gpwr.pin_bit_mask  = 1ULL << EPD_PWR_PIN;
    gpwr.pull_up_en    = GPIO_PULLUP_DISABLE;
    gpwr.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_config(&gpwr);

    gpio_config_t g = {};
    g.intr_type     = GPIO_INTR_DISABLE;
    g.mode          = GPIO_MODE_OUTPUT;
    g.pin_bit_mask  = (1ULL << cs_) | (1ULL << dc_) | (1ULL << rst_);
    g.pull_up_en    = GPIO_PULLUP_DISABLE;
    g.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_config(&g);
    g.mode         = GPIO_MODE_INPUT;
    g.pin_bit_mask = (1ULL << busy_);
    gpio_config(&g);
    gpio_set_level(rst_, 1);
    gpio_set_level(cs_, 1);  // CS 默认拉高(SPI device 不被选中)
}

void EpdSsd1683::ReadBusy() {
    // 5 s 超时兜底：屏挂死、排线松了时不会让刷新永久阻塞。正常全刷约 1.2 s、局刷约 0.8 s。
    // 超时直接重启，比挂死后等看门狗复位更可控，日志也更明确。
    constexpr int64_t kBusyTimeoutMs = 5000;
    const int64_t     start_ms       = time_utils::NowMs();
    while (gpio_get_level(busy_) == 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (time_utils::NowMs() - start_ms > kBusyTimeoutMs) {
            ESP_LOGE(kTag, "busy timeout elapsed_ms=%lld action=restart", kBusyTimeoutMs);
            esp_restart();
        }
    }
}

void EpdSsd1683::EpdSendCommand(uint8_t c) {
    gpio_set_level(dc_, 0);
    gpio_set_level(cs_, 0);
    spi_transaction_t t = {};
    t.length            = 8;
    t.tx_buffer         = &c;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(cs_, 1);
}

void EpdSsd1683::EpdSendData(uint8_t d) {
    gpio_set_level(dc_, 1);
    gpio_set_level(cs_, 0);
    spi_transaction_t t = {};
    t.length            = 8;
    t.tx_buffer         = &d;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(cs_, 1);
}

void EpdSsd1683::WriteBytes(const uint8_t* buf, int len) {
    gpio_set_level(dc_, 1);
    gpio_set_level(cs_, 0);
    spi_transaction_t t = {};
    t.length            = 8 * len;
    t.tx_buffer         = buf;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(cs_, 1);
}

void EpdSsd1683::EpdPowerOn() {
    GpioWriteHold(EPD_PWR_PIN, 1);
}

void EpdSsd1683::EpdPowerOff() {
    GpioWriteHold(EPD_PWR_PIN, 0);
}
