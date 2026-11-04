#pragma once

#include <string>
#include <vector>

// 内容库：tools 生成的 /c/library.json。路径都是相对 /c 的文件路径。
namespace library {

struct Frame {
    std::string img;
    int         ms = 0;  // 这一帧刷完后停留的时间
};

struct Action {
    std::vector<Frame> frames;
};

struct Vehicle {
    std::string         name;
    std::string         base;
    std::string         intro;
    std::vector<Action> actions;
};

struct Page {
    std::string img;
    std::string audio;
};

enum class Type { kStory, kVehicles };

struct Item {
    Type                 type = Type::kStory;
    std::string          id;
    std::string          name;   // 状态栏标题
    std::string          title;  // 标题朗读
    std::vector<Page>    pages;
    std::vector<Vehicle> vehicles;
};

struct Library {
    std::string       volume_sound;
    std::string       font16;  // 状态栏标题字库
    std::string       font12;  // 状态栏数字字库
    std::string       built;   // 内容构建时间，如 "11-04 09:30"
    std::vector<Item> items;
};

bool Load(const std::string& root, Library& out);

}  // namespace library
