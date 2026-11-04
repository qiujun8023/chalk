#pragma once

namespace power {

enum class WakeSource { kColdBoot, kEnter, kDown, kCharge, kOther };

// 启动时调用一次：读取本次的唤醒来源。
WakeSource DetectWakeSource();

// 关闭音频与屏幕供电、保持 VBAT 自锁后进入深睡。唤醒源：确认键、下键、插入 USB。
// 调用前需要已停止音频，且没有进行中的屏幕刷新。
[[noreturn]] void EnterDeepSleep();

}  // namespace power
