#include "app/app.h"

#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bsp/board.h"
#include "bsp/charge_status.h"
#include "drivers/audio/audio_player.h"
#include "ui/menu.h"
#include "utils/time_utils.h"

extern "C" const char kFirmwareVersion[];  // version.c，每次编译生成

namespace {
constexpr char kTag[]    = "app";
constexpr char kRoot[]   = "/c";
constexpr char kState[]  = "/c/state.json";
constexpr char kScreen[] = "/c/screen.bin";  // 深睡前屏幕上的整帧，唤醒后交给驱动作为上一帧

// 默认局刷；连续局刷到这个次数后，下一次切换内容时改用全刷清残影。
// 留出一次切换加一段动画（最多 4 帧）的余量，驱动的 16 次上限不会在动画中途强制全刷。列表移动只局刷。
constexpr int     kPartialBudget    = EpdSsd1683::kMaxPartialBeforeFull - 4;
constexpr int64_t kIdleSleepMs      = 3 * 60 * 1000;
constexpr int64_t kMenuTimeoutMs    = 30 * 1000;  // 列表 / 设置页无操作自动退出
constexpr int64_t kBatteryPollMs    = 60 * 1000;
constexpr int     kAutoAdvanceMs    = 1500;  // 朗读结束到自动翻页的停顿
constexpr int     kTitleGapMs       = 300;   // 念完组名 / 标题到下一段朗读的停顿
constexpr int     kVolumeMaxLevel   = 10;  // 音量 1–10 档，每档 10
constexpr int     kVolumeMinLevel   = 1;

// 设置页各行。
enum SettingsRow { kRowBack, kRowVolume, kSettingsRows };

// 列表和设置共用一种版式：一屏 7 行。设置页下方隔一道线列出三行只读的设备信息，贴近底部。
constexpr menu::Layout kMenuLayout{status_bar::kHeight + 4, EpdSsd1683::kHeight - 2, 38};
constexpr int          kInfoTop  = 200;
constexpr int          kInfoRowH = 30;
constexpr int          kInfoRule = kInfoTop - 16;

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

// 连按累加的只有上下（-1 / +1）。
bool Stackable(Ev ev) {
    return ev == Ev::kUp || ev == Ev::kDown;
}

int Sign(Ev ev) {
    return ev == Ev::kUp ? -1 : 1;
}

int Wrap(int i, int n) {
    return (i % n + n) % n;
}

int VolumeLevel() {
    return (AudioPlayer::Get().volume() + 5) / 10;
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
        input::SetImmediate(mode_ != Mode::kBrowse);
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

// 取下一个要处理的事件。播完、充电这类后台事件当场记下（安排自动播放 / 标记状态栏待刷新），不占位；
// 阻塞操作期间积压的按键只保留最后一个，连续的上下键累加步数，一次跳到目标、只刷一屏。
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
        if (have && Stackable(out.ev) && Stackable(e.ev)) {
            const int total = Sign(out.ev) * out.steps + Sign(e.ev) * e.steps;
            out             = {total < 0 ? Ev::kUp : Ev::kDown, 0, total < 0 ? -total : total};
        } else {
            out = e;
        }
        have = true;
    }
    return have;
}

void App::Restore(power::WakeSource wake) {
    const int groups = static_cast<int>(lib_.groups.size());
    positions_.assign(groups, 0);
    group_ = (saved_.group >= 0 && saved_.group < groups) ? saved_.group : 0;
    for (int g = 0; g < groups && g < static_cast<int>(saved_.positions.size()); ++g) {
        const int n   = static_cast<int>(lib_.groups[g].entries.size());
        positions_[g] = saved_.positions[g] < n ? saved_.positions[g] : 0;
    }
    page_ = IsStory() && saved_.page >= 0 && saved_.page < static_cast<int>(Current().pages.size()) ? saved_.page : 0;
    uint16_t mv  = 0;
    uint8_t  pct = 0;
    if (Board::Get().ReadBattery(&mv, &pct))
        battery_pct_ = pct;
    last_battery_ms_ = time_utils::NowMs();

    // 深睡期间画面一直保留：把它交给驱动作为上一帧，后续直接局刷。
    const bool seeded = wake != power::WakeSource::kColdBoot && ReadFrame(kScreen, frame_);
    if (seeded)
        epd_->Seed(frame_.data());
    ESP_LOGI(kTag, "restore wake=%d group=%d entry=%d page=%d seeded=%d", static_cast<int>(wake), group_,
             positions_[group_], page_, seeded ? 1 : 0);

    // 唤醒用的这次按键直接执行：确认是从头再播，下键是组内下一项。
    if (seeded && (wake == power::WakeSource::kEnter || wake == power::WakeSource::kDown)) {
        Handle({wake == power::WakeSource::kEnter ? Ev::kEnter : Ev::kDown, 0});
        return;
    }
    if (wake == power::WakeSource::kColdBoot)
        StartEntry(/*with_group_title=*/true);
    else
        ShowPage(/*speak=*/false);
}

void App::Handle(const Event& e) {
    if (input::IsButton(e.ev)) {
        last_input_ms_ = time_utils::NowMs();
        auto_at_ms_    = 0;  // 手动操作取消待执行的自动播放
        if (e.steps == 0)
            return;  // 连按相互抵消
    }
    switch (e.ev) {
        case Ev::kSleep: SleepNow(); return;
        case Ev::kInfo:
            ESP_LOGI(kTag,
                     "info mode=%d group=%d entry=%d page=%d shown=%s volume=%d battery=%d partial_since_full=%d "
                     "heap=%u",
                     static_cast<int>(mode_), group_, positions_[group_], page_, shown_.c_str(),
                     AudioPlayer::Get().volume(), battery_pct_, epd_->partial_since_full(),
                     static_cast<unsigned>(esp_get_free_heap_size()));
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
    if (mode_ == Mode::kBrowse) {
        HandleBrowse(e.ev, e.steps);
        return;
    }
    // 列表和设置页里长按就是一次短按。
    const Ev ev = e.ev == Ev::kUpLong ? Ev::kUp : e.ev == Ev::kDownLong ? Ev::kDown : e.ev;
    if (mode_ == Mode::kList)
        HandleList(ev, e.steps);
    else
        HandleSettings(ev, e.steps);
}

void App::HandleBrowse(Ev ev, int steps) {
    switch (ev) {
        case Ev::kUp: SwitchEntry(-steps); break;
        case Ev::kDown: SwitchEntry(steps); break;
        case Ev::kUpLong: SwitchGroup(-1); break;
        case Ev::kDownLong: SwitchGroup(1); break;
        case Ev::kEnter: StartEntry(/*with_group_title=*/false); break;
        case Ev::kList: EnterList(); break;
        case Ev::kSettings: EnterSettings(); break;
        default: break;
    }
}

// 列表：项层是当前组的各项，组层是各组；第一行「返回」，项层返回到组层，组层返回到内容。光标移动不朗读。
void App::HandleList(Ev ev, int steps) {
    const bool groups = list_level_ == ListLevel::kGroups;
    switch (ev) {
        case Ev::kUp:
        case Ev::kDown:
            list_cursor_ = Wrap(list_cursor_ + Sign(ev) * steps, ListRows());
            RenderMenu();
            break;
        case Ev::kEnter:
            if (groups && list_cursor_ == 0) {
                ExitMenu(/*resume=*/true);
            } else if (groups) {
                list_level_  = ListLevel::kEntries;
                list_group_  = list_cursor_ - 1;
                list_cursor_ = positions_[list_group_] + 1;
                RenderMenu();
            } else if (list_cursor_ == 0) {
                list_level_  = ListLevel::kGroups;
                list_cursor_ = list_group_ + 1;
                RenderMenu();
            } else {
                SelectEntry(list_group_, list_cursor_ - 1);
            }
            break;
        case Ev::kList: ExitMenu(/*resume=*/true); break;
        case Ev::kSettings: EnterSettings(); break;
        default: break;
    }
}

int App::ListRows() const {
    return 1 + static_cast<int>(list_level_ == ListLevel::kGroups ? lib_.groups.size()
                                                                  : lib_.groups[list_group_].entries.size());
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
            settings_cursor_ = Wrap(settings_cursor_ + Sign(ev) * steps, kSettingsRows);
            RenderMenu();
            return;
        case Ev::kEnter: break;
        case Ev::kSettings: ExitMenu(/*resume=*/true); return;
        default: return;
    }
    switch (settings_cursor_) {
        case kRowBack: ExitMenu(/*resume=*/true); break;
        case kRowVolume:
            editing_volume_ = !editing_volume_;
            RenderMenu();
            break;
    }
}

void App::SwitchEntry(int delta) {
    ESP_LOGI(kTag, "switch entry delta=%d group=%d from=%d", delta, group_, positions_[group_]);
    positions_[group_] = Wrap(positions_[group_] + delta, static_cast<int>(Group().entries.size()));
    StartEntry(/*with_group_title=*/false);
}

// 切组后播那一组记住的项。
void App::SwitchGroup(int delta) {
    ESP_LOGI(kTag, "switch group delta=%d from=%d", delta, group_);
    group_ = Wrap(group_ + delta, static_cast<int>(lib_.groups.size()));
    StartEntry(/*with_group_title=*/true);
}

void App::SelectEntry(int group, int entry) {
    const bool changed = group != group_;
    mode_              = Mode::kBrowse;
    group_             = group;
    positions_[group_] = entry;
    StartEntry(/*with_group_title=*/changed);
}

// 开场（组名、故事标题）念完再念第一页；没有开场时直接朗读，车同时播动画。
void App::StartEntry(bool with_group_title) {
    Stop();
    page_          = 0;
    action_cursor_ = 0;
    intro_.clear();
    if (with_group_title)
        intro_.push_back(Group().title_audio);
    if (IsStory() && !Current().title_audio.empty())
        intro_.push_back(Current().title_audio);
    if (intro_.empty()) {
        ShowPage(/*speak=*/true);
        return;
    }
    stage_           = Stage::kIntro;
    intro_next_      = 1;
    narration_token_ = Play(intro_[0]);
    ShowPage(/*speak=*/false);
}

// 先出声再刷屏，刷新的等待被朗读盖住；车在画面刷好后播动画。
void App::ShowPage(bool speak) {
    if (speak)
        Narrate();
    Render(IsStory() ? Current().pages[page_].img : Current().base, NextMode());
    if (speak && !IsStory())
        PlayAction();
}

void App::SpeakCurrent() {
    Narrate();
    if (!IsStory())
        PlayAction();
}

void App::Narrate() {
    stage_           = Stage::kPage;
    narration_token_ = Play(IsStory() ? Current().pages[page_].audio : Current().intro);
}

// 朗读播完：开场念完一段接着念下一段，全部念完开始播当前页；故事页念完就安排翻页，最后一页停下。
void App::OnAudioDone(uint32_t token) {
    if (mode_ != Mode::kBrowse || token == 0 || token != narration_token_)
        return;
    if (stage_ == Stage::kIntro) {
        Schedule(intro_next_ < intro_.size() ? Auto::kIntro : Auto::kSpeak, kTitleGapMs);
        return;
    }
    if (stage_ == Stage::kPage && IsStory() && page_ + 1 < static_cast<int>(Current().pages.size())) {
        Schedule(Auto::kNextPage, kAutoAdvanceMs);
        return;
    }
    stage_ = Stage::kIdle;
}

void App::Schedule(Auto what, int delay_ms) {
    auto_       = what;
    auto_at_ms_ = time_utils::NowMs() + delay_ms;
}

void App::RunAuto() {
    ESP_LOGI(kTag, "auto what=%d group=%d entry=%d page=%d", static_cast<int>(auto_), group_, positions_[group_],
             page_);
    const Auto what = auto_;
    auto_           = Auto::kNone;
    auto_at_ms_     = 0;
    last_input_ms_  = time_utils::NowMs();  // 自动播放期间不进深睡
    switch (what) {
        case Auto::kIntro: narration_token_ = Play(intro_[intro_next_++]); break;
        case Auto::kSpeak: SpeakCurrent(); break;
        case Auto::kNextPage:
            ++page_;
            ShowPage(/*speak=*/true);
            break;
        case Auto::kNone: break;
    }
}

void App::Stop() {
    stage_      = Stage::kIdle;
    auto_       = Auto::kNone;
    auto_at_ms_ = 0;
}

void App::PlayAction() {
    const library::Entry& v = Current();
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

    ESP_LOGI(kTag, "action done vehicle=%s frames=%u interrupted=%d elapsed_ms=%lld", v.name.c_str(),
             static_cast<unsigned>(action.frames.size()), interrupted ? 1 : 0,
             static_cast<long long>(time_utils::NowMs() - start_ms));
}

// 队列里有需要打断动画的事件就返回 true。播完和充电事件不打断：前者安排自动播放，后者等动画结束后刷新状态栏。
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

// 进入列表 / 设置页时停下朗读和自动播放。列表进入项层，光标停在当前项。
void App::EnterList() {
    mode_        = Mode::kList;
    list_level_  = ListLevel::kEntries;
    list_group_  = group_;
    list_cursor_ = positions_[group_] + 1;
    Stop();
    Play("");
    RenderMenu();
}

void App::EnterSettings() {
    mode_            = Mode::kSettings;
    settings_cursor_ = kRowVolume;
    editing_volume_  = false;
    Stop();
    Play("");
    RenderMenu();
}

void App::ExitMenu(bool resume) {
    ESP_LOGI(kTag, "exit menu mode=%d resume=%d", static_cast<int>(mode_), resume ? 1 : 0);
    mode_           = Mode::kBrowse;
    editing_volume_ = false;
    Stop();
    if (!resume)
        Play("");
    ShowPage(resume);
}

void App::ChangeVolume(int delta) {
    const int level = std::clamp(VolumeLevel() + delta, kVolumeMinLevel, kVolumeMaxLevel);
    AudioPlayer::Get().SetVolume(level * 10);
    ESP_LOGI(kTag, "volume level=%d", level);
    if (!lib_.volume_sound.empty())
        Play(lib_.volume_sound);
    RenderMenu();
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
    if (mode_ == Mode::kList) {
        s.caption = list_level_ == ListLevel::kGroups ? "目录" : lib_.groups[list_group_].name;
        if (list_cursor_ > 0)
            s.progress = std::to_string(list_cursor_) + "/" + std::to_string(ListRows() - 1);
    } else if (mode_ == Mode::kSettings) {
        s.caption = "设置";
    } else {
        s.progress = std::to_string(positions_[group_] + 1) + "/" + std::to_string(Group().entries.size());
        s.caption  = Current().name;
    }
    return s;
}

EpdSsd1683::Mode App::NextMode() const {
    return epd_->partial_since_full() >= kPartialBudget ? EpdSsd1683::Mode::kFull : EpdSsd1683::Mode::kPartial;
}

void App::Render(const std::string& img, EpdSsd1683::Mode mode) {
    if (!ReadFrame(Abs(img), frame_)) {
        ESP_LOGW(kTag, "image missing path=%s", img.c_str());
        return;
    }
    bar_ = CurrentBar();
    status_bar::Draw(frame_.data(), bar_, fonts_);
    epd_->Show(frame_.data(), mode);
    shown_ = img;
}

void App::RenderMenu(EpdSsd1683::Mode mode, bool only_if_changed) {
    frame_.assign(EpdSsd1683::kBufferLen, 0xFF);
    std::vector<menu::Row> rows;
    if (mode_ == Mode::kList) {
        rows.push_back(menu::Row::Back());
        if (list_level_ == ListLevel::kGroups) {
            for (const library::Group& g : lib_.groups)
                rows.push_back({g.name});
        } else {
            for (const library::Entry& e : lib_.groups[list_group_].entries)
                rows.push_back({e.name});
        }
        menu::DrawList(frame_.data(), fonts_.title, rows, list_cursor_, kMenuLayout);
    } else {
        rows = {
            menu::Row::Back(),
            {"音量", std::to_string(VolumeLevel()), editing_volume_},
        };
        menu::DrawList(frame_.data(), fonts_.title, rows, settings_cursor_, kMenuLayout);
        menu::DrawRule(frame_.data(), kInfoRule);
        menu::DrawPairs(frame_.data(), fonts_.title, InfoRows(), kInfoTop, kInfoRowH);
    }
    bar_ = CurrentBar();
    status_bar::Draw(frame_.data(), bar_, fonts_);
    if (only_if_changed && memcmp(frame_.data(), epd_->current(), frame_.size()) == 0)
        return;
    epd_->Show(frame_.data(), mode);
}

// 设置页下方的只读信息：电池（电量或充电状态，加电压）、存储用量、固件版本。
std::vector<std::pair<std::string, std::string>> App::InfoRows() const {
    uint16_t   mv  = 0;
    uint8_t    pct = 0;
    const bool ok  = Board::Get().ReadBattery(&mv, &pct);
    mv             = mv / 50 * 50;  // 只显示到 0.05V，避免 ADC 抖动导致频繁局刷
    const auto charge = Board::Get().charge()->Get();
    char       buf[32];
    // 充电时电压被充电 IC 拉高、百分比不可信，换成充电状态（与状态栏一致）。
    if (!ok)
        snprintf(buf, sizeof(buf), "--");
    else if (charge.full || charge.charging)
        snprintf(buf, sizeof(buf), "%s  %d.%02dV", charge.full ? "已充满" : "充电中", mv / 1000, mv % 1000 / 10);
    else
        snprintf(buf, sizeof(buf), "%d%%  %d.%02dV", (pct + 5) / 10 * 10, mv / 1000, mv % 1000 / 10);
    std::vector<std::pair<std::string, std::string>> rows{{"电池", buf}};
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    snprintf(buf, sizeof(buf), "已用 %.1f / %.1f MB", used / 1048576.0, total / 1048576.0);
    rows.push_back({"存储", buf});
    rows.push_back({"版本", kFirmwareVersion});
    return rows;
}

uint32_t App::Play(const std::string& audio) {
    if (audio.empty()) {
        AudioPlayer::Get().Stop();
        return 0;
    }
    return AudioPlayer::Get().Play(Abs(audio));
}

// 屏幕上保留的是内容页：在列表 / 设置页时先切回内容再睡。
void App::SleepNow() {
    AudioPlayer::Get().Stop();
    if (mode_ != Mode::kBrowse)
        ExitMenu(/*resume=*/false);
    vTaskDelay(pdMS_TO_TICKS(50));
    saved_.volume = AudioPlayer::Get().volume();
    saved_.group  = group_;
    saved_.page   = page_;
    saved_.positions.assign(positions_.begin(), positions_.end());
    saved_.Save(kState);
    WriteFrame(kScreen, epd_->current());
    power::EnterDeepSleep();
}
