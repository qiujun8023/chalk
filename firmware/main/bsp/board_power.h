#pragma once

// 系统级电源管理,管三条 audio 子系统相关 pin + VBAT 软锁存:
//   - VBAT 软锁存(GPIO17 拉低=关机的唯一手段)
//   - AVDD_3V3 / Audio rail(GPIO42,关掉=I²C 死、ES8311/PA/MIC 失能)
//   - PA CTRL(GPIO46,PA 数字使能,必须先于 audio rail 通电时被驱动 LOW)
// EPD_PWR(GPIO6) 由 EpdSsd1683 自管。
//
// 为什么 PA pin 也归这里管:audio rail 一通电 PA U5 就吃电,如果此时 PA CTRL
// 浮空可能被读为 HIGH → PA 放大 ES8311 默认 DC bias → 喇叭"啵"。所以 PA pin
// 必须跟 audio rail 在同一构造里 init,确保 PowerAudioOn 时 CTRL 已稳是 LOW。
// 后续由 AudioPlayer::EnsureCodecOpen 在 100 ms DAC 稳定窗后再拉高出声。
//
// LED(GPIO3):绿灯，低有效；只在 InitLed 里配成输出并熄灭，之后不再驱动。

class BoardPowerBsp {
   public:
    BoardPowerBsp(int audioPowerPin, int audioAmpPin, int vbatPowerPin);
    ~BoardPowerBsp() = default;

    // 一次性把 LED 配成 OUTPUT 并熄灭。可在 Init 序列任意点调，幂等。
    void InitLed();

    void PowerAudioOn();
    void PowerAudioOff();
    void VbatPowerOn();
    void VbatPowerOff();

   private:
    const int audioPowerPin_;
    const int audioAmpPin_;  // PA CTRL,构造时锁定 LOW
    const int vbatPowerPin_;
};
