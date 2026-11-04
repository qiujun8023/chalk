#pragma once

#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_oneshot.h>

#include <atomic>
#include <cstdint>

class BatteryAdc {
   public:
    BatteryAdc() = default;
    ~BatteryAdc();

    bool Init();
    bool Read(uint16_t* voltage_mv, uint8_t* percent);

   private:
    adc_oneshot_unit_handle_t adc_handle_  = nullptr;
    adc_cali_handle_t         cali_handle_ = nullptr;
    std::atomic<bool>         ready_{false};
};
