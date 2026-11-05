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

// 电池：1px 圆角外框 + 正极头，内部一根按电量比例填充的细条，四周留 1px 白边。
// 高度与数字的墨迹高度相同，和旁边的百分比上下对齐。
constexpr int kBodyW = 17, kCapW = 2, kBatteryW = kBodyW + kCapW;

int DrawBattery(uint8_t* fb, int right, int top, int h, int pct) {
    const int x = right - kBatteryW;
    Fill(fb, x + 1, top, kBodyW - 2, 1);           // 上边
    Fill(fb, x + 1, top + h - 1, kBodyW - 2, 1);   // 下边
    Fill(fb, x, top + 1, 1, h - 2);                // 左边
    Fill(fb, x + kBodyW - 1, top + 1, 1, h - 2);   // 右边
    Fill(fb, x + kBodyW, top + (h - 3) / 2, kCapW, 3);  // 正极
    constexpr int kInnerW = kBodyW - 4;
    if (pct > 0)
        Fill(fb, x + 2, top + 2, (kInnerW * pct + 50) / 100, h - 4);
    return x;
}

// 充电闪电 6×9，画在电池左侧。
constexpr int         kBoltW = 6, kBoltH = 9;
constexpr const char* kBolt[] = {
    "....##", "...##.", "..##..", ".##...", "######", "...##.", "..##..", ".##...", "##....",
};

int Percent(const StatusBarState& s) {
    const int pct = s.full ? 100 : s.battery_pct;
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

// 按墨迹垂直居中：返回绘制时传给 Draw 的 top，ink_top / ink_h 是墨迹在状态栏里的位置。
// sample 是代表这一类文字的字符，内容变化时位置不跳。
int InkCenter(const BitmapFont& f, const char* sample, int& ink_top, int& ink_h) {
    int top = 0, bottom = f.height();
    f.InkRows(sample, top, bottom);
    ink_h   = bottom - top;
    ink_top = (kHeight - 1 - ink_h) / 2;
    return ink_top - top;
}
}  // namespace

void Draw(uint8_t* fb, const StatusBarState& s, const StatusBarFonts& fonts) {
    memset(fb, 0xFF, kHeight * (kW / 8));
    memset(fb + (kHeight - 1) * (kW / 8), 0x00, kW / 8);  // 底部分隔线

    int       ink_top = 0, ink_h = 0;
    const int digits_top = InkCenter(fonts.digits, "0123456789%", ink_top, ink_h);
    if (!s.progress.empty())
        fonts.digits.Draw(fb, kPad, digits_top, s.progress);

    const int icon_x = DrawBattery(fb, kW - kPad, ink_top, ink_h, Percent(s));

    // 充电中电压被充电 IC 拉高、百分比不可信，换成闪电；充满显示 100%；未知显示 --。
    if (s.charging) {
        Pattern(fb, icon_x - kGap - kBoltW, ink_top + (ink_h - kBoltH) / 2, kBolt, kBoltH);
    } else {
        char pct[8] = "--";
        if (s.full || s.battery_pct >= 0)
            snprintf(pct, sizeof(pct), "%d%%", Percent(s));
        fonts.digits.Draw(fb, icon_x - kGap - fonts.digits.Measure(pct), digits_top, pct);
    }

    int       title_ink = 0, title_h = 0;
    const int title_top = InkCenter(fonts.title, "目", title_ink, title_h);  // 「目」来自固定文字「目录」，字库里一定有
    const int title_w   = fonts.title.Measure(s.caption);
    fonts.title.Draw(fb, (kW - title_w) / 2, title_top, s.caption);
}

}  // namespace status_bar
