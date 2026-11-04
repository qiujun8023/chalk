#pragma once

#include <cstdint>
#include <string>

#include "ui/font.h"

// 顶部 24px 状态栏，布局与 slate 相同：中间标题（Zfull 16px），右侧电量百分比（Zfull 12px）+ 电池图标，
// 底部一条分隔线。slate 左侧是 WiFi，这里不联网，换成进度。
struct StatusBarState {
    std::string progress;  // 如 "3/12"
    std::string caption;   // 如 "挖掘机"、"音量 7"
    int         battery_pct = -1;  // -1 = 未知
    bool        charging    = false;
    bool        full        = false;

    bool operator==(const StatusBarState&) const = default;
};

struct StatusBarFonts {
    BitmapFont title;   // 16px
    BitmapFont digits;  // 12px
};

namespace status_bar {

constexpr int kHeight = 24;

void Draw(uint8_t* fb, const StatusBarState& s, const StatusBarFonts& fonts);

}  // namespace status_bar
