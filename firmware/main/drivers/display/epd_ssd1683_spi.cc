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

// SPI 反复 free + reinit 是为了切换 DI 数据线方向(EPD 单数据线复用 MOSI/MISO):
// 写命令/数据走 mosi_io_num=mosi_(发送),读温度时 miso_io_num=mosi_(同一物理引脚反向接收)。

void EpdSsd1683::SpiPortInit() {
    if (spi_ && spi_inited_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_remove_device(spi_));
        spi_ = nullptr;
    }
    if (spi_inited_) {
        esp_err_t free_ret = spi_bus_free(spi_host_);
        if (free_ret != ESP_OK && free_ret != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(free_ret);
        }
        spi_inited_ = false;
    }
    spi_bus_config_t b              = {};
    b.miso_io_num                   = -1;
    b.mosi_io_num                   = mosi_;
    b.sclk_io_num                   = sclk_;
    b.quadwp_io_num                 = -1;
    b.quadhd_io_num                 = -1;
    b.max_transfer_sz               = kBufferLen * 2;
    spi_device_interface_config_t d = {};
    d.spics_io_num                  = -1;
    d.clock_speed_hz                = 40 * 1000 * 1000;
    d.mode                          = 0;
    d.queue_size                    = 7;
    ESP_ERROR_CHECK(spi_bus_initialize(spi_host_, &b, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(spi_host_, &d, &spi_));
    spi_inited_ = true;
}

void EpdSsd1683::SpiPortRxInit() {
    if (spi_ && spi_inited_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_remove_device(spi_));
        spi_ = nullptr;
    }
    if (spi_inited_) {
        esp_err_t free_ret = spi_bus_free(spi_host_);
        if (free_ret != ESP_OK && free_ret != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(free_ret);
        }
        spi_inited_ = false;
    }
    spi_bus_config_t b              = {};
    b.miso_io_num                   = mosi_;  // DI 反向当 MISO 收数据
    b.mosi_io_num                   = -1;
    b.sclk_io_num                   = sclk_;
    b.quadwp_io_num                 = -1;
    b.quadhd_io_num                 = -1;
    b.max_transfer_sz               = kBufferLen * 2;
    spi_device_interface_config_t d = {};
    d.spics_io_num                  = -1;
    d.clock_speed_hz                = 8 * 1000 * 1000;  // 读时降速到 8 MHz
    d.mode                          = 0;
    d.queue_size                    = 7;
    ESP_ERROR_CHECK(spi_bus_initialize(spi_host_, &b, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(spi_host_, &d, &spi_));
    spi_inited_ = true;
}

uint8_t EpdSsd1683::EpdRecvData() {
    SpiPortRxInit();
    uint8_t           rx = 0;
    spi_transaction_t t  = {};
    t.length             = 8;
    t.rx_buffer          = &rx;
    gpio_set_level(dc_, 1);
    gpio_set_level(cs_, 0);
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(cs_, 1);
    SpiPortInit();
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
    // 5s 超时兜底:屏挂死/带线松了不会让刷新永久阻塞。
    // 正常 full 刷 ~3s,partial ~1s,5s 留足余量。超时直接 panic 重启,
    // 比挂死 + WDT 复位更可控,日志也更明确。
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
