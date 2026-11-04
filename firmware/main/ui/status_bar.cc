#include "ui/status_bar.h"

#include <cstdio>
#include <cstring>

#include "drivers/display/epd_ssd1683.h"

namespace status_bar {

namespace {
constexpr int kW   = EpdSsd1683::kWidth;
constexpr int kPad = 8;
constexpr int kGap = 4;  // 百分比与电池图标的间距

void Pixel(uint8_t* fb, int x, int y) {
    fb[y * (kW / 8) + x / 8] &= ~(0x80 >> (x % 8));
}

void Fill(uint8_t* fb, int x, int y, int w, int h) {
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx)
            Pixel(fb, xx, yy);
}

void Pattern(uint8_t* fb, int x, int y, const char* const* rows, int n) {
    for (int r = 0; r < n; ++r)
        for (int c = 0; rows[r][c]; ++c)
            if (rows[r][c] == '#')
                Pixel(fb, x + c, y + r);
}

// 电池 23×11：圆角外框 + 正极头，内部三格电量。返回图标左边界。
constexpr int kBatteryW = 23, kBatteryH = 11;

int DrawBattery(uint8_t* fb, int right, int top, int segments) {
    const int x = right - kBatteryW;
    Fill(fb, x + 1, top, 19, 1);               // 上边
    Fill(fb, x + 1, top + kBatteryH - 1, 19, 1);  // 下边
    Fill(fb, x, top + 1, 1, kBatteryH - 2);       // 左边
    Fill(fb, x + 20, top + 1, 1, kBatteryH - 2);  // 右边
    Fill(fb, x + 21, top + 3, 2, 5);              // 正极
    for (int i = 0; i < segments; ++i)
        Fill(fb, x + 2 + i * 6, top + 2, 5, kBatteryH - 4);
    return x;
}

// 充电闪电 7×11，画在电池左侧。
constexpr int         kBoltW   = 7;
constexpr const char* kBolt[] = {
    ".....##", "....##.", "...##..", "..##...", ".######", "######.", "...##..", "..##...", ".##....", "##.....", "#......",
};

int Segments(const StatusBarState& s) {
    const int pct = s.full ? 100 : s.battery_pct;
    if (pct < 0)
        return 0;
    return pct >= 67 ? 3 : pct >= 34 ? 2 : pct >= 10 ? 1 : 0;
}

int CenterTop(const BitmapFont& f) {
    return (kHeight - 1 - f.height()) / 2;
}
}  // namespace

void Draw(uint8_t* fb, const StatusBarState& s, const StatusBarFonts& fonts) {
    memset(fb, 0xFF, kHeight * (kW / 8));
    memset(fb + (kHeight - 1) * (kW / 8), 0x00, kW / 8);  // 底部分隔线

    if (!s.progress.empty())
        fonts.digits.Draw(fb, kPad, CenterTop(fonts.digits), s.progress);

    const int icon_x = DrawBattery(fb, kW - kPad, (kHeight - 1 - kBatteryH) / 2, Segments(s));

    // 充满显示 100%；充电中电压被充电 IC 拉高、百分比不可信，换成闪电；未知显示 --。
    char pct[8] = "";
    if (s.charging && !s.full) {
        Pattern(fb, icon_x - kGap - kBoltW, (kHeight - 1 - 11) / 2, kBolt, 11);
    } else if (s.full)
        snprintf(pct, sizeof(pct), "100%%");
    else if (s.battery_pct < 0)
        snprintf(pct, sizeof(pct), "--");
    else
        snprintf(pct, sizeof(pct), "%d%%", s.battery_pct > 100 ? 100 : s.battery_pct);
    if (pct[0])
        fonts.digits.Draw(fb, icon_x - kGap - fonts.digits.Measure(pct), CenterTop(fonts.digits), pct);

    const int title_w = fonts.title.Measure(s.caption);
    fonts.title.Draw(fb, (kW - title_w) / 2, CenterTop(fonts.title), s.caption);
}

}  // namespace status_bar
