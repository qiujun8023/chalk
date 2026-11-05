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
#include "ui/status_bar.h"

// 界面状态机，跑在单独的 task 里。内容分组（工程车、小故事、古诗），平时显示当前组当前项（车或故事）的画面；
// 上下短按组内切换，长按切组，确认从头再播；上下同按进列表，三键同按进设置。
// 刷屏是阻塞的，期间到达的按键只保留最后一个。
class App {
   public:
    void Run(power::WakeSource wake);

   private:
    enum class Mode { kBrowse, kList, kSettings };
    enum class ListLevel { kEntries, kGroups };  // 列表先显示当前组的各项，「返回」到组列表
    // 播放阶段：先念完开场（组名、标题），再念当前页。
    enum class Stage { kIdle, kIntro, kPage };
    // 朗读结束后到 auto_at_ms_ 时要做的事。
    enum class Auto { kNone, kIntro, kSpeak, kNextPage };

    void Restore(power::WakeSource wake);
    void Handle(const Event& e);
    void HandleBrowse(Ev ev, int steps);
    void HandleList(Ev ev, int steps);
    void HandleSettings(Ev ev, int steps);

    void SwitchEntry(int delta);  // 组内上一项 / 下一项
    void SwitchGroup(int delta);  // 上一组 / 下一组
    void SelectEntry(int group, int entry);
    // 从第一页开始播当前项：先依次念 intro（组名、故事标题），再念第一页。
    void StartEntry(bool with_group_title);
    void ShowPage(bool speak);  // 画当前页；speak 时开始朗读（车还播动画）
    void SpeakCurrent();        // 画面已在屏上，只开始朗读和动画
    void Narrate();             // 开始念当前页（车是介绍）
    void OnAudioDone(uint32_t token);
    void Schedule(Auto what, int delay_ms);
    void RunAuto();
    void PlayAction();
    bool HasPendingButton();
    bool WaitButton(int ms);  // 等待 ms，期间有按键返回 true
    void EnterList();
    void EnterSettings();
    void ExitMenu(bool resume);  // 回到内容；resume 时接着朗读 / 播放当前页
    void Stop();                 // 停下朗读和待执行的自动播放
    void ChangeVolume(int delta);
    void RefreshStatusBar();
    void SleepNow();

    // 画一帧：内容图 + 状态栏。
    void             Render(const std::string& img, EpdSsd1683::Mode mode);
    // 画列表 / 设置页，默认局刷（残影换响应速度，与 slate 相同）；only_if_changed 时与屏上画面相同就不刷。
    void             RenderMenu(EpdSsd1683::Mode mode = EpdSsd1683::Mode::kPartial, bool only_if_changed = false);
    std::vector<std::pair<std::string, std::string>> InfoRows() const;
    EpdSsd1683::Mode NextMode() const;
    StatusBarState   CurrentBar() const;
    uint32_t         Play(const std::string& audio);
    bool             WaitNext(Event& out, TickType_t timeout);
    int              ListRows() const;  // 列表行数，含「返回」

    const library::Group& Group() const {
        return lib_.groups[group_];
    }
    const library::Entry& Current() const {
        return Group().entries[positions_[group_]];
    }
    bool IsStory() const {
        return Current().kind == library::Kind::kStory;
    }

    QueueHandle_t        queue_ = nullptr;
    library::Library     lib_;
    SavedState           saved_;
    StatusBarFonts       fonts_;
    EpdSsd1683*          epd_   = nullptr;
    Mode                 mode_  = Mode::kBrowse;
    int                  group_ = 0;
    std::vector<int>     positions_;  // 每组的当前项
    int                  page_          = 0;  // 当前项（故事）的当前页
    int                  action_cursor_ = 0;
    ListLevel            list_level_    = ListLevel::kEntries;
    int                  list_group_    = 0;  // 列表项层显示的组
    int                  list_cursor_   = 0;  // 0 是「返回」，i 是第 i-1 行
    int                  list_top_      = 0;  // 列表视口第一行，光标移出时逐行滚动
    int                  settings_cursor_ = 0;
    bool                 editing_volume_  = false;
    uint32_t             narration_token_ = 0;
    std::string          shown_;  // 当前内容图（在列表 / 设置页时是退出后要回到的那张）
    StatusBarState       bar_;    // 屏幕上当前的状态栏
    std::vector<uint8_t> frame_;
    int64_t              last_input_ms_   = 0;
    int64_t              last_battery_ms_ = 0;
    int                  battery_pct_     = -1;
    bool                 charge_dirty_    = false;

    Stage                    stage_ = Stage::kIdle;
    std::vector<std::string> intro_;  // 开场要念的音频
    size_t                   intro_next_ = 0;
    Auto                     auto_       = Auto::kNone;
    int64_t                  auto_at_ms_ = 0;
};
