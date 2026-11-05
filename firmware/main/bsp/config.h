#pragma once

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_codec_dev_defaults.h>

#include <cstdint>

// 板级 pinout / 常量。命名风格统一 UPPER_SNAKE,与 ESP-IDF Kconfig 风格一致。
// 本文件不引入运行时逻辑,只供其他 .cc 直接 #include "config.h"。

// ── Audio (ES8311) ─────────────────────────────────────────────
constexpr int AUDIO_OUTPUT_SAMPLE_RATE = 16000;

constexpr gpio_num_t AUDIO_I2S_GPIO_MCLK = GPIO_NUM_14;
constexpr gpio_num_t AUDIO_I2S_GPIO_WS   = GPIO_NUM_38;
constexpr gpio_num_t AUDIO_I2S_GPIO_BCLK = GPIO_NUM_15;
constexpr gpio_num_t AUDIO_I2S_GPIO_DOUT = GPIO_NUM_45;

constexpr gpio_num_t AUDIO_CODEC_PA_PIN      = GPIO_NUM_46;
constexpr gpio_num_t AUDIO_CODEC_I2C_SDA_PIN = GPIO_NUM_47;
constexpr gpio_num_t AUDIO_CODEC_I2C_SCL_PIN = GPIO_NUM_48;
constexpr uint8_t    AUDIO_CODEC_ES8311_ADDR = ES8311_CODEC_DEFAULT_ADDR;

// ── Buttons ────────────────────────────────────────────────────
constexpr gpio_num_t BOOT_BUTTON_GPIO = GPIO_NUM_0;
constexpr gpio_num_t UP_BUTTON_GPIO   = GPIO_NUM_39;
constexpr gpio_num_t DOWN_BUTTON_GPIO = GPIO_NUM_18;  // 与 POWER_KEY_GPIO 复用:开机时电源键 = 下键

// ── Charge IC ──────────────────────────────────────────────────
constexpr gpio_num_t CHARGE_DETECT_GPIO           = GPIO_NUM_2;
constexpr gpio_num_t CHARGE_FULL_GPIO             = GPIO_NUM_1;
constexpr int        CHARGE_DETECT_CHARGING_LEVEL = 0;  // 0=低有效充电,1=高有效

// ── EPD (SSD1683) ──────────────────────────────────────────────
constexpr spi_host_device_t EPD_SPI_NUM = SPI3_HOST;

constexpr gpio_num_t EPD_DC_PIN   = GPIO_NUM_10;
constexpr gpio_num_t EPD_CS_PIN   = GPIO_NUM_11;
constexpr gpio_num_t EPD_SCK_PIN  = GPIO_NUM_12;
constexpr gpio_num_t EPD_MOSI_PIN = GPIO_NUM_13;
constexpr gpio_num_t EPD_RST_PIN  = GPIO_NUM_9;
constexpr gpio_num_t EPD_BUSY_PIN = GPIO_NUM_8;

// ── 系统电源 ──────────────────────────────────────────────────
constexpr gpio_num_t EPD_PWR_PIN           = GPIO_NUM_6;   // EPD 电源,EpdSsd1683 自管
constexpr gpio_num_t AUDIO_PWR_PIN         = GPIO_NUM_42;       // AVDD_3V3 rail (I²C 上拉也在这条 rail)
constexpr gpio_num_t VBAT_PWR_PIN          = GPIO_NUM_17;       // 系统软锁存,拉低 = 关机
constexpr gpio_num_t POWER_KEY_GPIO        = DOWN_BUTTON_GPIO;  // SW1=下键开机,Board::InitPower 等其松开
