#include "ui/menu.h"

#include <algorithm>
#include <cstdlib>

#include "drivers/display/epd_ssd1683.h"

namespace menu {

namespace {
constexpr int kW      = EpdSsd1683::kWidth;
constexpr int kLeft   = 12;
constexpr int kRight  = kW - 16;  // 右侧留给滚动条
constexpr int kTextX  = kLeft + 16;
constexpr int kValueR = kRight - 16;
constexpr int kArrowW = 5, kArrowH = 9, kArrowGap = 6;

void Set(uint8_t* fb, int x, int y, bool white) {
    uint8_t&      byte = fb[y * (kW / 8) + x / 8];
    const uint8_t mask = 0x80 >> (x % 8);
    byte               = white ? (byte | mask) : (byte & ~mask);
}

void Fill(uint8_t* fb, int x, int y, int w, int h, bool white) {
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx)
            Set(fb, xx, yy, white);
}

// 实心三角，left 为 true 时尖朝左。
void Arrow(uint8_t* fb, int x, int y, bool left, bool white) {
    for (int r = 0; r < kArrowH; ++r) {
        const int len = kArrowW - std::abs(r - kArrowH / 2);  // 中间一行最长
        for (int i = 0; i < len; ++i)
            Set(fb, left ? x + kArrowW - 1 - i : x + i, y + r, white);
    }
}
}  // namespace

void DrawRule(uint8_t* fb, int y) {
    Fill(fb, kLeft, y, kRight - kLeft, 1, false);
}

void DrawList(uint8_t* fb, const BitmapFont& font, const std::vector<Row>& rows, int cursor, const Layout& layout) {
    const int n       = static_cast<int>(rows.size());
    const int visible = std::max(1, (layout.bottom - layout.top) / layout.row_h);
    const int first   = cursor / visible * visible;
    const int text_dy = (layout.row_h - font.height()) / 2;

    for (int i = first; i < n && i < first + visible; ++i) {
        const int  y   = layout.top + (i - first) * layout.row_h;
        const bool sel = i == cursor;
        if (sel)
            Fill(fb, kLeft, y, kRight - kLeft, layout.row_h - 2, false);
        const Row& row = rows[i];
        font.Draw(fb, kTextX, y + text_dy, row.label, sel);
        if (row.value.empty())
            continue;
        const int vw = font.Measure(row.value);
        int       vx = kValueR - vw;
        if (row.editing) {
            vx -= kArrowW + kArrowGap;
            const int ay = y + (layout.row_h - 2 - kArrowH) / 2;
            Arrow(fb, vx - kArrowGap - kArrowW, ay, true, sel);
            Arrow(fb, vx + vw + kArrowGap, ay, false, sel);
        }
        font.Draw(fb, vx, y + text_dy, row.value, sel);
    }

    // 滚动条：只有一页放不下时才画，滑块位置和长度对应当前页。
    if (n > visible) {
        const int track_x = kW - 8, track_h = layout.bottom - layout.top;
        Fill(fb, track_x + 1, layout.top, 1, track_h, false);
        const int pages = (n + visible - 1) / visible;
        const int th    = std::max(8, track_h / pages);
        const int ty    = layout.top + (track_h - th) * (first / visible) / std::max(1, pages - 1);
        Fill(fb, track_x, ty, 3, th, false);
    }
}

}  // namespace menu
