#include "bsp/battery_adc.h"

#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <esp_rom_sys.h>

namespace {
constexpr char kTag[] = "battery_adc";
}  // namespace

BatteryAdc::~BatteryAdc() {
    ready_.store(false, std::memory_order_release);
    if (cali_handle_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(adc_cali_delete_scheme_curve_fitting(cali_handle_));
        cali_handle_ = nullptr;
    }
    if (adc_handle_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(adc_oneshot_del_unit(adc_handle_));
        adc_handle_ = nullptr;
    }
}

bool BatteryAdc::Init() {
    if (ready_.load(std::memory_order_acquire))
        return true;

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1, .clk_src = ADC_RTC_CLK_SRC_DEFAULT, .ulp_mode = ADC_ULP_MODE_DISABLE};
    if (adc_oneshot_new_unit(&init_cfg, &adc_handle_) != ESP_OK) {
        ESP_LOGE(kTag, "adc unit create failed");
        return false;
    }
    adc_oneshot_chan_cfg_t ch = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    if (adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_3, &ch) != ESP_OK) {
        ESP_LOGE(kTag, "adc channel config failed");
        return false;
    }
    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_3, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    if (adc_cali_create_scheme_curve_fitting(&cali, &cali_handle_) != ESP_OK) {
        ESP_LOGE(kTag, "adc calibration create failed");
        return false;
    }

    ready_.store(true, std::memory_order_release);
    return true;
}

bool BatteryAdc::Read(uint16_t* voltage_mv, uint8_t* percent) {
    if (!ready_.load(std::memory_order_acquire))
        return false;

    int sum = 0;
    int n   = 0;
    for (int i = 0; i < 10; ++i) {
        int raw = 0;
        int mv  = 0;
        if (adc_oneshot_read(adc_handle_, ADC_CHANNEL_3, &raw) != ESP_OK)
            continue;
        if (adc_cali_raw_to_voltage(cali_handle_, raw, &mv) != ESP_OK)
            continue;
        sum += mv * 2;  // 板上 1:2 分压,×2 还原电池电压
        n++;
        if (i != 9)
            esp_rom_delay_us(100);
    }
    if (n == 0) {
        ESP_LOGW(kTag, "read failed reason=all_samples_failed");
        return false;
    }

    const int avg = sum / n;
    // 二阶多项式拟合:4200mV→100,3800mV→67,3300mV→0(单节锂电放电曲线)
    int p = (-1 * avg * avg + 9016 * avg - 19189000) / 10000;
    p     = p > 100 ? 100 : (p < 0 ? 0 : p);
    if (voltage_mv)
        *voltage_mv = static_cast<uint16_t>(avg);
    if (percent)
        *percent = static_cast<uint8_t>(p);
    return true;
}
