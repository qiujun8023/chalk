#pragma once

#include <driver/i2c_master.h>

#include <memory>

class BatteryAdc;
class BoardPowerBsp;
class ChargeStatus;
class Button;
class EpdSsd1683;

// 板级单例：电源、I²C、充电状态、电池 ADC、按键、EPD。
class Board {
   public:
    static Board& Get();

    void Init();

    BoardPowerBsp* power() {
        return power_.get();
    }
    ChargeStatus* charge() {
        return charge_.get();
    }
    Button* up_btn() {
        return up_btn_.get();
    }
    Button* down_btn() {
        return down_btn_.get();
    }
    Button* enter_btn() {
        return enter_btn_.get();
    }
    EpdSsd1683* epd() {
        return epd_.get();
    }
    i2c_master_bus_handle_t i2c_bus() {
        return i2c_bus_;
    }

    // 电池电压和百分比；没装电池或 ADC 未就绪时返回 false。
    bool ReadBattery(uint16_t* voltage_mv, uint8_t* percent);

   private:
    Board() = default;
    void InitPower();
    void InitI2c();
    void InitButtons();

    std::unique_ptr<BoardPowerBsp> power_;
    std::unique_ptr<ChargeStatus>  charge_;
    std::unique_ptr<BatteryAdc>    battery_;
    std::unique_ptr<Button>        up_btn_;
    std::unique_ptr<Button>        down_btn_;
    std::unique_ptr<Button>        enter_btn_;
    std::unique_ptr<EpdSsd1683>    epd_;
    i2c_master_bus_handle_t        i2c_bus_ = nullptr;
};
