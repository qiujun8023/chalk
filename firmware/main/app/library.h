#pragma once

#include <string>
#include <vector>

// 内容库：tools 生成的 /c/library.json。路径都是相对 /c 的文件路径。
// 内容分组（工程车、小动物、小故事、古诗），组里每一项是一个故事 / 一首诗，或一个形象（一辆车、一只动物）。
namespace library {

struct Frame {
    std::string img;
    int         ms = 0;  // 这一帧刷完后停留的时间
};

struct Page {
    std::string img;
    std::string audio;
};

// kFigure：一张原图 + 介绍朗读 + 一段关键帧动画。
enum class Kind { kStory, kFigure };

struct Entry {
    Kind        kind = Kind::kStory;
    std::string name;         // 状态栏标题、列表行
    std::string title_audio;  // 故事：标题朗读；形象：空（介绍里已有名字）

    std::vector<Page> pages;  // 故事

    std::string        base;  // 形象：原图
    std::string        intro;
    std::vector<Frame> frames;
};

struct Group {
    std::string        name;         // 列表行、状态栏标题
    std::string        title_audio;  // 组名朗读
    std::vector<Entry> entries;
};

struct Library {
    std::string        volume_sound;
    std::string        font16;  // 状态栏标题字库
    std::string        font12;  // 状态栏数字字库
    std::vector<Group> groups;
};

bool Load(const std::string& root, Library& out);

}  // namespace library
