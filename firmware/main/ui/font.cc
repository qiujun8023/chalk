#include "ui/font.h"

#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "drivers/display/epd_ssd1683.h"

namespace {
constexpr char kTag[] = "font";

uint32_t Le32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace

uint32_t NextCodepoint(const std::string& s, size_t& i) {
    const auto c = static_cast<uint8_t>(s[i]);
    int        n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (i + n > s.size())
        n = 1;
    uint32_t cp = n == 1 ? c : c & (0x7F >> n);
    for (int k = 1; k < n; ++k)
        cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
    i += n;
    return cp;
}

bool BitmapFont::Load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        ESP_LOGE(kTag, "open failed path=%s", path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    std::vector<uint8_t> raw(ftell(f));
    fseek(f, 0, SEEK_SET);
    const bool ok = fread(raw.data(), 1, raw.size(), f) == raw.size();
    fclose(f);
    constexpr size_t kHeader = 8, kEntry = 13;
    if (!ok || raw.size() < kHeader || memcmp(raw.data(), "CFN1", 4) != 0) {
        ESP_LOGE(kTag, "bad font path=%s", path.c_str());
        return false;
    }
    height_            = raw[4];
    const size_t count = raw[6] | (raw[7] << 8);
    if (raw.size() < kHeader + count * kEntry)
        return false;
    glyphs_.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* e = raw.data() + kHeader + i * kEntry;
        glyphs_[i]       = {Le32(e), e[4], e[5], e[6], static_cast<int8_t>(e[7]), static_cast<int8_t>(e[8]), Le32(e + 9)};
    }
    bits_.assign(raw.begin() + kHeader + count * kEntry, raw.end());
    return true;
}

const BitmapFont::Glyph* BitmapFont::Find(uint32_t cp) const {
    auto it = std::lower_bound(glyphs_.begin(), glyphs_.end(), cp, [](const Glyph& g, uint32_t c) { return g.cp < c; });
    return (it != glyphs_.end() && it->cp == cp) ? &*it : nullptr;
}

int BitmapFont::Measure(const std::string& utf8) const {
    int w = 0;
    for (size_t i = 0; i < utf8.size();) {
        if (const Glyph* g = Find(NextCodepoint(utf8, i)))
            w += g->adv;
    }
    return w;
}

bool BitmapFont::InkRows(const std::string& utf8, int& top, int& bottom) const {
    bool any = false;
    for (size_t i = 0; i < utf8.size();) {
        const Glyph* g = Find(NextCodepoint(utf8, i));
        if (!g)
            continue;
        // 字形位图可能带空白行，逐行看有没有像素。
        const int bpr = (g->w + 7) / 8;
        for (int r = 0; r < g->h && g->off + (r + 1) * bpr <= bits_.size(); ++r) {
            const uint8_t* row = bits_.data() + g->off + r * bpr;
            if (std::all_of(row, row + bpr, [](uint8_t b) { return b == 0; }))
                continue;
            const int y = g->y + r;
            top         = any ? std::min(top, y) : y;
            bottom      = any ? std::max(bottom, y + 1) : y + 1;
            any         = true;
        }
    }
    return any;
}

int BitmapFont::Draw(uint8_t* fb, int x, int top, const std::string& utf8, bool white) const {
    constexpr int W   = EpdSsd1683::kWidth;
    constexpr int H   = EpdSsd1683::kHeight;
    const int     x0  = x;
    for (size_t i = 0; i < utf8.size();) {
        const Glyph* g = Find(NextCodepoint(utf8, i));
        if (!g)
            continue;
        const int bpr = (g->w + 7) / 8;
        for (int r = 0; r < g->h; ++r) {
            const int py = top + g->y + r;
            if (py < 0 || py >= H || g->off + (r + 1) * bpr > bits_.size())
                continue;
            const uint8_t* row = bits_.data() + g->off + r * bpr;
            for (int c = 0; c < g->w; ++c) {
                const int px = x + g->x + c;
                if (px < 0 || px >= W || !(row[c / 8] & (0x80 >> (c % 8))))
                    continue;
                uint8_t&      byte = fb[py * (W / 8) + px / 8];
                const uint8_t mask = 0x80 >> (px % 8);
                byte               = white ? (byte | mask) : (byte & ~mask);
            }
        }
        x += g->adv;
    }
    return x - x0;
}
