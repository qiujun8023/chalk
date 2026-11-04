#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 深睡前保存、启动时恢复的状态，存在 LittleFS。
struct SavedState {
    int                   volume = 70;
    int                   group  = 0;
    std::vector<uint16_t> positions;  // 每组的当前页 / 当前车

    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
};
