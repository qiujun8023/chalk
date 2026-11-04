#include "app/app.h"

#include <esp_app_desc.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bsp/board.h"
#include "bsp/charge_status.h"
#include "drivers/audio/audio_player.h"
#include "ui/menu.h"
#include "utils/time_utils.h"

namespace {
constexpr char kTag[]    = "app";
constexpr char kRoot[]   = "/c";
constexpr char kState[]  = "/c/state.json";
constexpr char kScreen[] = "/c/screen.bin";  // 深睡前屏幕上的整帧，唤醒后交给驱动作为上一帧

// 默认局刷；连续局刷到这个次数后，下一次切换画面时改用全刷清残影。
constexpr int     kPartialBudget    = 8;
constexpr int64_t kIdleSleepMs      = 3 * 60 * 1000;
constexpr int64_t kMenuTimeoutMs    = 30 * 1000;  // 组列表 / 设置页无操作自动退出
constexpr int64_t kBatteryPollMs    = 60 * 1000;
constexpr int     kAutoAdvanceMs    = 1500;  // 朗读结束到自动翻页的停顿
constexpr int     kTitleGapMs       = 300;   // 切组时念完组名到开始播放的停顿
constexpr int     kVolumeLevels     = 10;  // 音量 1–10 档，每档 10
constexpr int     kVolumeMinLevel   = 1;

// 设置页各行。
enum SettingsRow { kRowBack, kRowVolume, kRowRefresh, kRowSleep, kRowReset, kSettingsRows };

constexpr menu::Layout kListLayout{status_bar::kHeight + 4, EpdSsd1683::kHeight - 2, 30};
constexpr menu::Layout kSettingsLayout{status_bar::kHeight + 4, status_bar::kHeight + 4 + kSettingsRows * 30, 30};
constexpr int          kFooterTop  = kSettingsLayout.bottom + 10;
constexpr int          kFooterLine = 24;

std::string Abs(const std::string& rel) {
    return std::string(kRoot) + "/" + rel;
}

bool ReadFrame(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out.resize(EpdSsd1683::kBufferLen);
    const size_t n = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return n == out.size();
}

bool WriteFrame(const std::string& path, const uint8_t* data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = fwrite(data, 1, EpdSsd1683::kBufferLen, f) == EpdSsd1683::kBufferLen;
    fclose(f);
    return ok;
}

// 编译日期 "Nov  4 2026" → "11-04"。
std::string MonthDay(const char* date) {
    static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char*       m         = strstr(kMonths, std::string(date, 3).c_str());
    const int         month     = m ? static_cast<int>(m - kMonths) / 3 + 1 : 0;
    char              buf[16];
    snprintf(buf, sizeof(buf), "%02d-%02d", month, atoi(date + 4));
    return buf;
}

// 可累加的按键：上下是一组（-1 / +1），确认单独一组（只在浏览内容时累加）；其他返回 0。
int Axis(Ev ev, bool browse) {
    return (ev == Ev::kUp || ev == Ev::kDown) ? 1 : (ev == Ev::kEnter && browse) ? 2 : 0;
}

int Sign(Ev ev) {
    return ev == Ev::kUp ? -1 : 1;
}
}  // namespace

void App::Run(power::WakeSource wake) {
    epd_   = Board::Get().epd();
    queue_ = xQueueCreate(8, sizeof(Event));
    if (!library::Load(kRoot, lib_) || !fonts_.title.Load(Abs(lib_.font16)) || !fonts_.digits.Load(Abs(lib_.font12))) {
        ESP_LOGE(kTag, "content missing; nothing to show");
        vTaskDelay(portMAX_DELAY);
    }
    saved_.Load(kState);
    AudioPlayer::Get().OnDone([q = queue_](uint32_t token) {
        Event e{Ev::kAudioDone, token};
        xQueueSend(q, &e, 0);
    });
    Board::Get().charge()->OnStateChanged([q = queue_](const ChargeStatus::Snapshot&) {
        Event e{Ev::kCharge};
        xQueueSend(q, &e, 0);
    });
    AudioPlayer::Get().SetVolume(saved_.volume);
    input::Install(queue_);
    last_input_ms_ = time_utils::NowMs();

    Restore(wake);

    while (true) {
        int64_t wait_ms = 1000;
        if (auto_at_ms_ > 0)
            wait_ms = std::clamp<int64_t>(auto_at_ms_ - time_utils::NowMs(), 0, 1000);
        Event e{};
        if (WaitNext(e, pdMS_TO_TICKS(wait_ms)))
            Handle(e);
        if (charge_dirty_)
            RefreshStatusBar();
        const int64_t now = time_utils::NowMs();
        if (auto_at_ms_ > 0 && now >= auto_at_ms_) {
            RunAuto();
            continue;
        }
        if (mode_ != Mode::kBrowse && now - last_input_ms_ >= kMenuTimeoutMs)
            ExitMenu(/*resume=*/false);
        if (now - last_battery_ms_ >= kBatteryPollMs)
            RefreshStatusBar();
        const bool idle = now - last_input_ms_ >= kIdleSleepMs;
        if (idle && !AudioPlayer::Get().IsPlaying() && !Board::Get().charge()->Get().power_present)
            SleepNow();
    }
}

// 取下一个要处理的事件。播完、充电这类后台事件当场记下（安排自动翻页 / 标记状态栏待刷新），不占位；
// 阻塞操作期间积压的按键只保留最后一个，连续的同类键累加步数，一次跳到目标、只刷一屏。
bool App::WaitNext(Event& out, TickType_t timeout) {
    bool       have = false;
    Event      e{};
    TickType_t wait = timeout;
    while (xQueueReceive(queue_, &e, wait) == pdTRUE) {
        wait = 0;
        if (e.ev == Ev::kAudioDone) {
            OnAudioDone(e.token);
            continue;
        }
        if (e.ev == Ev::kCharge) {
            charge_dirty_ = true;
            continue;
        }
        const bool browse = mode_ == Mode::kBrowse;
        const int  axis   = have ? Axis(out.ev, browse) : 0;
        if (axis != 0 && axis == Axis(e.ev, browse)) {
            const int total = Sign(out.ev) * out.steps + Sign(e.ev) * e.steps;
            out             = {axis == 2 ? Ev::kEnter : total < 0 ? Ev::kUp : Ev::kDown, 0, total < 0 ? -total : total};
        } else {
            out = e;
        }
        have = true;
    }
    return have;
}

void App::Restore(power::WakeSource wake) {
    positions_.assign(lib_.items.size(), 0);
    const int groups = static_cast<int>(lib_.items.size());
    group_           = (saved_.group >= 0 && saved_.group < groups) ? saved_.group : 0;
    for (int g = 0; g < groups && g < static_cast<int>(saved_.positions.size()); ++g) {
        const library::Item& it = lib_.items[g];
        const int n = static_cast<int>(it.type == library::Type::kStory ? it.pages.size() : it.vehicles.size());
        positions_[g] = saved_.positions[g] < n ? saved_.positions[g] : 0;
    }
    uint16_t mv = 0;
    uint8_t  pct = 0;
    if (Board::Get().ReadBattery(&mv, &pct))
        battery_pct_ = pct;
    last_battery_ms_ = time_utils::NowMs();

    // 深睡期间画面一直保留：把它交给驱动作为上一帧，后续直接局刷。
    const bool seeded = wake != power::WakeSource::kColdBoot && ReadFrame(kScreen, frame_);
    if (seeded)
        epd_->Seed(frame_.data());
    ESP_LOGI(kTag, "restore wake=%d group=%d pos=%d seeded=%d", static_cast<int>(wake), group_, Position(),
             seeded ? 1 : 0);

    // 唤醒用的这次按键直接执行：确认是组内下一个，下键是下一组。
    if (seeded && (wake == power::WakeSource::kEnter || wake == power::WakeSource::kDown)) {
        Handle({wake == power::WakeSource::kEnter ? Ev::kEnter : Ev::kDown, 0});
        return;
    }
    ShowCurrent(/*speak=*/wake == power::WakeSource::kColdBoot);
}

int App::Count() const {
    return Group().type == library::Type::kStory ? static_cast<int>(Group().pages.size())
                                                  : static_cast<int>(Group().vehicles.size());
}

void App::Handle(const Event& e) {
    if (input::IsButton(e.ev)) {
        last_input_ms_ = time_utils::NowMs();
        auto_at_ms_    = 0;  // 手动操作取消待执行的自动翻页
    }
    if (input::IsButton(e.ev) && e.steps == 0)
        return;  // 连按相互抵消
    switch (e.ev) {
        case Ev::kSleep: SleepNow(); return;
        case Ev::kInfo:
            ESP_LOGI(kTag, "info mode=%d group=%d pos=%d shown=%s volume=%d battery=%d partial_since_full=%d heap=%u",
                     static_cast<int>(mode_), group_, Position(), shown_.c_str(), AudioPlayer::Get().volume(),
                     battery_pct_, epd_->partial_since_full(), static_cast<unsigned>(esp_get_free_heap_size()));
            return;
        case Ev::kDump: {
            // 每行一行像素：FB:<行号>:<50 字节十六进制>，tools/devkeys.py --dump 还原成 PNG。
            const uint8_t* fb = epd_->current();
            char           line[8 + EpdSsd1683::kWidth / 8 * 2 + 2];
            for (int y = 0; y < EpdSsd1683::kHeight; ++y) {
                int n = snprintf(line, sizeof(line), "FB:%03d:", y);
                for (int i = 0; i < EpdSsd1683::kWidth / 8; ++i)
                    n += snprintf(line + n, sizeof(line) - n, "%02x", fb[y * (EpdSsd1683::kWidth / 8) + i]);
                printf("%s\n", line);
            }
            fflush(stdout);
            return;
        }

        default: break;
    }
    switch (mode_) {
        case Mode::kBrowse: HandleBrowse(e.ev, e.steps); break;
        case Mode::kList: HandleList(e.ev, e.steps); break;
        case Mode::kSettings: HandleSettings(e.ev, e.steps); break;
    }
}

void App::HandleBrowse(Ev ev, int steps) {
    switch (ev) {
        case Ev::kUp: SwitchGroup(-steps); break;
        case Ev::kDown: SwitchGroup(steps); break;
        case Ev::kEnter: Step(steps); break;
        case Ev::kList: EnterList(); break;
        case Ev::kSettings: EnterSettings(); break;
        default: break;
    }
}

// 组列表：第一行「返回」，之后每组一行。光标移到哪一组就念组名。
void App::HandleList(Ev ev, int steps) {
    const int n = static_cast<int>(lib_.items.size()) + 1;
    switch (ev) {
        case Ev::kUp:
        case Ev::kDown: {
            list_cursor_ = ((list_cursor_ + Sign(ev) * steps) % n + n) % n;
            Play(list_cursor_ > 0 ? lib_.items[list_cursor_ - 1].title : "");
            RenderMenu(NextMode());
            break;
        }
        case Ev::kEnter:
            if (list_cursor_ == 0)
                ExitMenu(/*resume=*/true);
            else
                SelectGroup(list_cursor_ - 1);
            break;
        case Ev::kList: ExitMenu(/*resume=*/true); break;
        case Ev::kSettings: EnterSettings(); break;
        default: break;
    }
}

// 设置页：上下移动光标，确认执行选中项；调音量时上下改值，再按确认结束。
void App::HandleSettings(Ev ev, int steps) {
    switch (ev) {
        case Ev::kUp:
        case Ev::kDown:
            if (editing_volume_) {
                ChangeVolume(-Sign(ev) * steps);  // 上调大，下调小
                return;
            }
            settings_cursor_ = ((settings_cursor_ + Sign(ev) * steps) % kSettingsRows + kSettingsRows) % kSettingsRows;
            reset_armed_     = false;
            RenderMenu(NextMode());
            return;
        case Ev::kEnter: break;
        case Ev::kSettings: ExitMenu(/*resume=*/true); return;
        default: return;
    }
    switch (settings_cursor_) {
        case kRowBack: ExitMenu(/*resume=*/true); break;
        case kRowVolume:
            editing_volume_ = !editing_volume_;
            RenderMenu(NextMode());
            break;
        case kRowRefresh: RenderMenu(EpdSsd1683::Mode::kFull); break;
        case kRowSleep: SleepNow(); break;
        case kRowReset:
            if (!reset_armed_) {
                reset_armed_ = true;
                RenderMenu(NextMode());
            } else {
                ResetProgress();
            }
            break;
    }
}

void App::Step(int delta) {
    ESP_LOGI(kTag, "step delta=%d group=%d pos=%d", delta, group_, Position());
    const int n    = Count();
    Position()     = ((Position() + delta) % n + n) % n;
    action_cursor_ = 0;
    ShowCurrent(true);
}

void App::SwitchGroup(int delta) {
    ESP_LOGI(kTag, "switch group delta=%d from=%d", delta, group_);
    const int n = static_cast<int>(lib_.items.size());
    SelectGroup(((group_ + delta) % n + n) % n);
}

// 切组从第一页开始：先念组名，念完再播第一页（故事接着自动翻页）。
void App::SelectGroup(int group) {
    mode_            = Mode::kBrowse;
    group_           = group;
    Position()       = 0;
    action_cursor_   = 0;
    narration_token_ = Play(Group().title);
    title_playing_   = true;
    ShowCurrent(false);
}

// 朗读播完：组名念完就开始播当前页；故事页念完就安排自动翻页，最后一页停下。工程车不自动换车。
void App::OnAudioDone(uint32_t token) {
    if (mode_ != Mode::kBrowse || token == 0 || token != narration_token_)
        return;
    if (title_playing_) {
        title_playing_ = false;
        auto_at_ms_    = time_utils::NowMs() + kTitleGapMs;
        auto_speak_    = true;
        return;
    }
    if (Group().type == library::Type::kStory && Position() + 1 < Count()) {
        auto_at_ms_ = time_utils::NowMs() + kAutoAdvanceMs;
        auto_speak_ = false;
    }
}

void App::RunAuto() {
    ESP_LOGI(kTag, "auto speak=%d group=%d pos=%d", auto_speak_ ? 1 : 0, group_, Position());
    auto_at_ms_    = 0;
    last_input_ms_ = time_utils::NowMs();  // 自动播放期间不进深睡
    if (auto_speak_)
        SpeakCurrent();
    else
        Step(1);
}

// 先出声再刷屏：朗读立即开始，刷新的那一秒被声音盖住。工程车在刷完原图后播动画。
void App::ShowCurrent(bool speak) {
    title_playing_ = title_playing_ && !speak;
    const bool story = Group().type == library::Type::kStory;
    if (speak)
        narration_token_ = Play(story ? Group().pages[Position()].audio : Group().vehicles[Position()].intro);
    Render(story ? Group().pages[Position()].img : Group().vehicles[Position()].base, NextMode());
    if (speak && !story)
        PlayAction();
}

// 画面已经在屏上（切组念完组名后），只开始朗读和动画。
void App::SpeakCurrent() {
    const bool story = Group().type == library::Type::kStory;
    narration_token_ = Play(story ? Group().pages[Position()].audio : Group().vehicles[Position()].intro);
    if (!story)
        PlayAction();
}

void App::PlayAction() {
    const library::Vehicle& v = Group().vehicles[Position()];
    if (v.actions.empty())
        return;
    const library::Action& action = v.actions[action_cursor_ % v.actions.size()];
    action_cursor_                = (action_cursor_ + 1) % static_cast<int>(v.actions.size());
    const int64_t start_ms        = time_utils::NowMs();
    // 动画期间有按键就停下，交给主循环处理那次按键（下一屏会直接覆盖当前帧）。
    bool interrupted = false;
    for (const library::Frame& f : action.frames) {
        if ((interrupted = HasPendingButton()))
            break;
        Render(f.img, EpdSsd1683::Mode::kPartial);
        if (f.ms > 0 && (interrupted = WaitButton(f.ms)))
            break;
    }

    // 残影预算用完时，在动作结束后补一次全刷，不在动作中途打断；有按键在等就留给下一屏。
    if (epd_->partial_since_full() >= kPartialBudget && !HasPendingButton())
        Render(shown_, EpdSsd1683::Mode::kFull);
    ESP_LOGI(kTag, "action done vehicle=%s frames=%u interrupted=%d elapsed_ms=%lld", v.name.c_str(),
             static_cast<unsigned>(action.frames.size()), interrupted ? 1 : 0,
             static_cast<long long>(time_utils::NowMs() - start_ms));
}

// 队列里有需要打断动画的事件就返回 true。播完和充电事件不打断：前者安排自动翻页，后者等动画结束后刷新状态栏。
bool App::HasPendingButton() {
    Event e{};
    while (xQueuePeek(queue_, &e, 0) == pdTRUE) {
        if (e.ev != Ev::kAudioDone && e.ev != Ev::kCharge)
            return true;
        xQueueReceive(queue_, &e, 0);
        if (e.ev == Ev::kAudioDone)
            OnAudioDone(e.token);
        else
            charge_dirty_ = true;
    }
    return false;
}

bool App::WaitButton(int ms) {
    const int64_t until = time_utils::NowMs() + ms;
    while (time_utils::NowMs() < until) {
        if (HasPendingButton())
            return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return HasPendingButton();
}

// 进入组列表 / 设置页时停下朗读和自动播放；组列表先念当前组名。
void App::EnterList() {
    mode_          = Mode::kList;
    title_playing_ = false;
    list_cursor_   = group_ + 1;
    Play(Group().title);
    RenderMenu(NextMode());
}

void App::EnterSettings() {
    mode_            = Mode::kSettings;
    title_playing_   = false;
    settings_cursor_ = kRowVolume;
    editing_volume_  = false;
    reset_armed_     = false;
    Play("");
    RenderMenu(NextMode());
}

void App::ExitMenu(bool resume) {
    ESP_LOGI(kTag, "exit menu mode=%d resume=%d", static_cast<int>(mode_), resume ? 1 : 0);
    mode_           = Mode::kBrowse;
    editing_volume_ = false;
    reset_armed_    = false;
    if (!resume)
        Play("");
    ShowCurrent(resume);
}

// 各组回到第一页，音量恢复默认，回到第一组。
void App::ResetProgress() {
    ESP_LOGI(kTag, "reset progress");
    std::fill(positions_.begin(), positions_.end(), 0);
    AudioPlayer::Get().SetVolume(SavedState{}.volume);
    saved_.volume = AudioPlayer::Get().volume();
    saved_.group  = 0;
    saved_.positions.assign(positions_.begin(), positions_.end());
    saved_.Save(kState);
    editing_volume_ = false;
    reset_armed_    = false;
    SelectGroup(0);
}

void App::ChangeVolume(int delta) {
    int level = (AudioPlayer::Get().volume() + 5) / 10 + delta;
    if (level < kVolumeMinLevel)
        level = kVolumeMinLevel;
    if (level > kVolumeLevels)
        level = kVolumeLevels;
    AudioPlayer::Get().SetVolume(level * 10);
    ESP_LOGI(kTag, "volume level=%d", level);
    if (!lib_.volume_sound.empty())
        Play(lib_.volume_sound);
    RenderMenu(NextMode());
}

// 定期读电量；状态栏（设置页还有底部的电量信息）有变化时局刷一次。
void App::RefreshStatusBar() {
    last_battery_ms_ = time_utils::NowMs();
    charge_dirty_    = false;
    uint16_t mv      = 0;
    uint8_t  pct     = 0;
    battery_pct_     = Board::Get().ReadBattery(&mv, &pct) ? pct : -1;
    if (mode_ != Mode::kBrowse) {
        RenderMenu(EpdSsd1683::Mode::kPartial, /*only_if_changed=*/true);
        return;
    }
    const StatusBarState next = CurrentBar();
    if (!shown_.empty() && !(next == bar_)) {
        ESP_LOGI(kTag, "status bar changed pct=%d->%d charging=%d->%d full=%d->%d", bar_.battery_pct, next.battery_pct,
                 bar_.charging, next.charging, bar_.full, next.full);
        Render(shown_, EpdSsd1683::Mode::kPartial);
    }
}

StatusBarState App::CurrentBar() const {
    StatusBarState s;
    const auto     charge = Board::Get().charge()->Get();
    s.charging            = charge.charging && !charge.full;
    s.full                = charge.full;
    s.battery_pct         = battery_pct_;
    // 电量按 10% 取整，避免 ADC 抖动导致频繁局刷。
    if (s.battery_pct >= 0)
        s.battery_pct = (s.battery_pct + 5) / 10 * 10;
    const int pos = positions_[group_];
    if (mode_ == Mode::kList) {
        s.caption = "目录";
        if (list_cursor_ > 0)
            s.progress = std::to_string(list_cursor_) + "/" + std::to_string(lib_.items.size());
    } else if (mode_ == Mode::kSettings) {
        s.caption = "设置";
    } else {
        s.progress = std::to_string(pos + 1) + "/" + std::to_string(Count());
        s.caption  = Group().type == library::Type::kStory ? Group().name : Group().vehicles[pos].name;
    }
    return s;
}

EpdSsd1683::Mode App::NextMode() const {
    return epd_->partial_since_full() >= kPartialBudget ? EpdSsd1683::Mode::kFull : EpdSsd1683::Mode::kPartial;
}

bool App::Render(const std::string& img, EpdSsd1683::Mode mode) {
    if (!ReadFrame(Abs(img), frame_)) {
        ESP_LOGW(kTag, "image missing path=%s", img.c_str());
        return false;
    }
    bar_ = CurrentBar();
    status_bar::Draw(frame_.data(), bar_, fonts_);
    epd_->Show(frame_.data(), mode);
    shown_ = img;
    return true;
}

void App::RenderMenu(EpdSsd1683::Mode mode, bool only_if_changed) {
    frame_.assign(EpdSsd1683::kBufferLen, 0xFF);
    std::vector<menu::Row> rows;
    if (mode_ == Mode::kList) {
        rows.push_back({"返回"});
        for (const library::Item& it : lib_.items)
            rows.push_back({it.name});
        menu::DrawList(frame_.data(), fonts_.title, rows, list_cursor_, kListLayout);
    } else {
        const int level = (AudioPlayer::Get().volume() + 5) / 10;
        rows            = {
            {"返回"},
            {"音量", std::to_string(level), editing_volume_},
            {"刷新屏幕"},
            {"立即休眠"},
            {reset_armed_ ? "再按确认重置" : "重置进度"},
        };
        menu::DrawList(frame_.data(), fonts_.title, rows, settings_cursor_, kSettingsLayout);
        DrawSettingsFooter(frame_.data());
    }
    bar_ = CurrentBar();
    status_bar::Draw(frame_.data(), bar_, fonts_);
    if (only_if_changed && memcmp(frame_.data(), epd_->current(), frame_.size()) == 0)
        return;
    epd_->Show(frame_.data(), mode);
}

// 设置页底部的只读信息：电量、存储、内容和固件的构建时间。
void App::DrawSettingsFooter(uint8_t* fb) {
    menu::DrawRule(fb, kFooterTop - 6);
    uint16_t   mv = 0;
    uint8_t    pct = 0;
    const bool ok = Board::Get().ReadBattery(&mv, &pct);
    mv            = mv / 50 * 50;  // 只显示到 0.05V，避免 ADC 抖动导致频繁局刷
    const auto  charge = Board::Get().charge()->Get();
    const char* state  = charge.full ? "已充满" : charge.charging ? "充电中" : "电池供电";
    char        line[64];
    if (ok)
        snprintf(line, sizeof(line), "电量 %d%%  %d.%02dV  %s", (pct + 5) / 10 * 10, mv / 1000, mv % 1000 / 10, state);
    else
        snprintf(line, sizeof(line), "电量 --  %s", state);
    const int x = 28;
    fonts_.title.Draw(fb, x, kFooterTop, line);

    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    snprintf(line, sizeof(line), "存储 %.1f/%.1fMB", used / 1048576.0, total / 1048576.0);
    fonts_.title.Draw(fb, x, kFooterTop + kFooterLine, line);

    const esp_app_desc_t* app = esp_app_get_description();
    snprintf(line, sizeof(line), "内容 %s  固件 %s %.5s", lib_.built.c_str(), MonthDay(app->date).c_str(), app->time);
    fonts_.title.Draw(fb, x, kFooterTop + kFooterLine * 2, line);
}

uint32_t App::Play(const std::string& audio) {
    if (audio.empty()) {
        AudioPlayer::Get().Stop();
        return 0;
    }
    return AudioPlayer::Get().Play(Abs(audio));
}

// 屏幕上保留的是内容页：在组列表 / 设置页时先切回内容再睡。
void App::SleepNow() {
    AudioPlayer::Get().Stop();
    if (mode_ != Mode::kBrowse)
        ExitMenu(/*resume=*/false);
    vTaskDelay(pdMS_TO_TICKS(50));
    saved_.volume = AudioPlayer::Get().volume();
    saved_.group  = group_;
    saved_.positions.assign(positions_.begin(), positions_.end());
    saved_.Save(kState);
    WriteFrame(kScreen, epd_->current());
    power::EnterDeepSleep();
}
