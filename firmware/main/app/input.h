#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstdint>

enum class Ev : uint8_t {
    kUp,
    kDown,
    kEnter,
    kUpLong,     // 上 长按：上一组
    kDownLong,   // 下 长按：下一组
    kList,       // 上 + 下 同按：进入 / 退出组列表
    kSettings,   // 三键同按：进入 / 退出设置
    kAudioDone,  // 音频自然播完，token 指明是哪一次播放
    kCharge,     // 充电状态变化（插拔 USB、充满）
    kSleep,      // 调试：立即深睡
    kInfo,       // 调试：打印状态
    kDump,       // 调试：把屏幕当前帧以十六进制打印到串口
};

struct Event {
    Ev       ev;
    uint32_t token = 0;  // kAudioDone：哪一次播放
    int      steps = 1;  // kUp / kDown：合并后的连按次数
};

namespace input {

// 绑定三个按键和 USB 串口调试输入，事件投递到 queue。
void Install(QueueHandle_t queue);

// on 时按键在按下时就触发（列表、设置页用）；否则短按在松手时触发，按住到长按时间触发长按。
void SetImmediate(bool on);

bool IsButton(Ev ev);

}  // namespace input
