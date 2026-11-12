#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ui/font.h"

// 状态栏下方的文字列表（内容列表、设置），一行一项，选中行左侧画黑条；
// 放不下时视口跟着光标逐行滚动，右侧画滚动条。
namespace menu {

struct Row {
    Row(std::string l, std::string v = "", bool e = false) : label(std::move(l)), value(std::move(v)), editing(e) {
    }

    static Row Back() {
        Row r("返回");
        r.back = true;
        return r;
    }

    std::string label;
    std::string value;         // 右对齐，如音量档位
    bool        editing;       // 值两侧画 ◀ ▶，表示上下键正在调这个值
    bool        back = false;  // 「返回」行：文字前画 ◀，和内容行区分
};

struct Layout {
    int top;       // 列表区域顶边
    int bottom;    // 列表区域底边（不含）
    int row_h;
};

// fb 需已清成白底。cursor 为 -1 时不画光标。
// top 是视口第一行，调用方跨帧保存；光标移出视口时只滚到刚好能看见光标，结果写回 top。
void DrawList(uint8_t* fb, const BitmapFont& font, const std::vector<Row>& rows, int cursor, int& top,
              const Layout& layout);

// 水平分隔线。
void DrawRule(uint8_t* fb, int y);

// 只读信息：每行「名称 数值」，两列左对齐，名称列与列表文字对齐。top 是第一行文字的顶边。
void DrawPairs(uint8_t* fb, const BitmapFont& font, const std::vector<std::pair<std::string, std::string>>& rows,
               int top, int row_h);

}  // namespace menu
