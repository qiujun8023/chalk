#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ui/font.h"

// 状态栏下方的文字列表（组列表、设置），一行一项，选中行反色；放不下时按页显示，右侧画滚动条。
namespace menu {

struct Row {
    Row(std::string l, std::string v = "", bool e = false) : label(std::move(l)), value(std::move(v)), editing(e) {
    }

    std::string label;
    std::string value;    // 右对齐，如音量档位
    bool        editing;  // 值两侧画 ◀ ▶，表示上下键正在调这个值
};

struct Layout {
    int top;       // 列表区域顶边
    int bottom;    // 列表区域底边（不含）
    int row_h;
};

// fb 需已清成白底。
void DrawList(uint8_t* fb, const BitmapFont& font, const std::vector<Row>& rows, int cursor, const Layout& layout);

// 水平分隔线。
void DrawRule(uint8_t* fb, int y);

}  // namespace menu
