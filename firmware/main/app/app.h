#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <string>
#include <vector>

#include "app/input.h"
#include "app/library.h"
#include "app/saved_state.h"
#include "drivers/display/epd_ssd1683.h"
#include "power/sleep.h"
#include "ui/font.h"
#include "ui/status_bar.h"

// 界面状态机，跑在单独的 task 里。内容按组（工程车、绘本……）组织，平时显示某一组的当前页 / 当前车；
// 上下同按进组列表，三键同按进设置。刷屏是阻塞的，期间到达的按键只保留最后一个。
class App {
   public:
    void Run(power::WakeSource wake);

   private:
    enum class Mode { kBrowse, kList, kSettings };

    void Restore(power::WakeSource wake);
    void Handle(const Event& e);
    void HandleBrowse(Ev ev, int steps);
    void HandleList(Ev ev, int steps);
    void HandleSettings(Ev ev, int steps);

    void Step(int delta);         // 组内下一个
    void SwitchGroup(int delta);  // 上一组 / 下一组
    void SelectGroup(int group);  // 切到某一组，从第一页开始
    void ShowCurrent(bool speak);
    void SpeakCurrent();
    void OnAudioDone(uint32_t token);
    void RunAuto();
    void PlayAction();
    bool HasPendingButton();
    bool WaitButton(int ms);  // 等待 ms，期间有按键返回 true
    void EnterList();
    void EnterSettings();
    void ExitMenu(bool resume);  // 回到内容；resume 时接着朗读 / 播放当前页
    void ChangeVolume(int delta);
    void ResetProgress();
    void RefreshStatusBar();
    void SleepNow();

    // 画一帧：内容图 + 状态栏。
    bool             Render(const std::string& img, EpdSsd1683::Mode mode);
    // 画组列表 / 设置页；only_if_changed 时与屏上画面相同就不刷。
    void             RenderMenu(EpdSsd1683::Mode mode, bool only_if_changed = false);
    void             DrawSettingsFooter(uint8_t* fb);
    EpdSsd1683::Mode NextMode() const;
    StatusBarState   CurrentBar() const;
    uint32_t         Play(const std::string& audio);
    bool             WaitNext(Event& out, TickType_t timeout);

    const library::Item& Group() const {
        return lib_.items[group_];
    }
    int& Position() {
        return positions_[group_];
    }
    int Count() const;

    QueueHandle_t        queue_ = nullptr;
    library::Library     lib_;
    SavedState           saved_;
    StatusBarFonts       fonts_;
    EpdSsd1683*          epd_   = nullptr;
    Mode                 mode_  = Mode::kBrowse;
    int                  group_ = 0;
    std::vector<int>     positions_;
    int                  action_cursor_   = 0;
    int                  list_cursor_     = 0;  // 0 是「返回」，i 是第 i-1 组
    int                  settings_cursor_ = 0;
    bool                 editing_volume_  = false;
    bool                 reset_armed_     = false;  // 重置进度已按过一次确认
    uint32_t             narration_token_ = 0;
    std::string          shown_;  // 当前内容图（在组列表 / 设置页时是退出后要回到的那张）
    StatusBarState       bar_;    // 屏幕上当前的状态栏
    std::vector<uint8_t> frame_;
    int64_t              last_input_ms_   = 0;
    int64_t              last_battery_ms_ = 0;
    int                  battery_pct_     = -1;
    bool                 charge_dirty_    = false;

    // 自动播放：朗读结束后到 auto_at_ms_ 时翻到下一页；auto_speak_ 表示只开始播当前页（切组念完组名后）。
    int64_t auto_at_ms_    = 0;
    bool    auto_speak_    = false;
    bool    title_playing_ = false;
};
