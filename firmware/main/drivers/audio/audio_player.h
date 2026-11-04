#pragma once

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// I2S + ES8311 输出，播放 LittleFS 里的 Ogg Opus 文件，边解码边写 I2S。
// Play 会打断当前播放。播放自然结束时回调 on_done(token)，被打断或 Stop 不回调。
class AudioPlayer {
   public:
    static AudioPlayer& Get();

    bool Init(i2c_master_bus_handle_t i2c_bus, int volume);

    // 返回本次播放的 token；文件读取失败返回 0。
    uint32_t Play(const std::string& path);
    void     Stop();
    bool     IsPlaying() const {
        return playing_.load(std::memory_order_acquire);
    }

    void SetVolume(int v);
    int  volume() const {
        return volume_.load(std::memory_order_relaxed);
    }

    void OnDone(std::function<void(uint32_t token)> cb) {
        on_done_ = std::move(cb);
    }

   private:
    AudioPlayer() = default;
    static void TaskEntry(void* arg);
    void        TaskLoop();
    bool        EnsureCodecOpen(int sample_rate);
    void        CloseCodec();
    void        SetCodecVolume(int v);
    // 解码并播放一段 Ogg 数据；返回 true 表示自然播完。
    bool        Decode(const std::vector<uint8_t>& ogg);
    bool        WritePcm(const uint8_t* pcm, size_t len, bool& unmuted, bool need_unmute);

    bool                         initialized_ = false;
    int                          codec_rate_  = 0;  // 0 = codec 未打开
    bool                         played_once_ = false;
    i2s_chan_handle_t            tx_handle_   = nullptr;
    const audio_codec_data_if_t* data_if_     = nullptr;
    const audio_codec_ctrl_if_t* ctrl_if_     = nullptr;
    const audio_codec_if_t*      codec_if_    = nullptr;
    const audio_codec_gpio_if_t* gpio_if_     = nullptr;
    esp_codec_dev_handle_t       dev_         = nullptr;
    std::atomic<int>             volume_{70};

    SemaphoreHandle_t     mutex_  = nullptr;  // 保护 pending_*
    SemaphoreHandle_t     notify_ = nullptr;
    std::vector<uint8_t>  pending_;
    bool                  has_pending_   = false;
    uint32_t              pending_token_ = 0;
    uint32_t              next_token_    = 1;
    std::atomic<bool>     stop_flag_{false};
    std::atomic<bool>     playing_{false};
    std::atomic<uint32_t> current_token_{0};
    std::function<void(uint32_t)> on_done_;
    TaskHandle_t                  task_ = nullptr;
};
