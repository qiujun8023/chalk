#pragma once

#include <cstdint>
#include <string>
#include <vector>

// tools 生成的点阵字库（格式见 tools/font.go），绘制到 1bpp 帧缓冲（bit=1 白）。
class BitmapFont {
   public:
    bool Load(const std::string& path);

    int height() const {
        return height_;
    }
    int Measure(const std::string& utf8) const;
    // 这串字实际有像素的行范围（相对行框顶边，bottom 不含），用于按墨迹而不是行框对齐；没有可画的字返回 false。
    bool InkRows(const std::string& utf8, int& top, int& bottom) const;
    // 在 (x, top) 处绘制一行字（默认黑字，white 为白字），top 是行框顶边；返回绘制宽度。缺字跳过。
    int Draw(uint8_t* fb, int x, int top, const std::string& utf8, bool white = false) const;

   private:
    struct Glyph {
        uint32_t cp;
        uint8_t  adv, w, h;
        int8_t   x, y;
        uint32_t off;
    };
    const Glyph* Find(uint32_t cp) const;

    int                  height_ = 0;
    std::vector<Glyph>   glyphs_;
    std::vector<uint8_t> bits_;
};

// UTF-8 解码一个码点并前移 i；非法字节按单字节跳过。
uint32_t NextCodepoint(const std::string& s, size_t& i);
