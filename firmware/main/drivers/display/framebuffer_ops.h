#pragma once

#include <cstdint>

namespace epd {

// SSD2683 每像素 2bit。全刷只写新帧（低位）；局刷高位放上一帧、低位放新帧。
inline uint16_t ExpandBits(uint8_t in, int shift) {
    uint16_t out = 0;
    for (int bit = 0; bit < 8; ++bit)
        out |= static_cast<uint16_t>((in >> bit) & 1) << (2 * bit + shift);
    return out;
}

inline void Pack1bppTo2683(uint8_t in, uint8_t& out0, uint8_t& out1) {
    const uint16_t v = ExpandBits(in, 0);
    out0             = static_cast<uint8_t>(v >> 8);
    out1             = static_cast<uint8_t>(v & 0xFF);
}

inline void PackPartial1bppTo2683(uint8_t prev, uint8_t now, uint8_t& out0, uint8_t& out1) {
    const uint16_t v = ExpandBits(prev, 1) | ExpandBits(now, 0);
    out0             = static_cast<uint8_t>(v >> 8);
    out1             = static_cast<uint8_t>(v & 0xFF);
}

}  // namespace epd
