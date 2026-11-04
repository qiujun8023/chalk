#include "drivers/audio/audio_player.h"

#include <decoder/esp_audio_dec_default.h>
#include <esp_codec_dev_defaults.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <simple_dec/esp_audio_simple_dec.h>
#include <simple_dec/esp_audio_simple_dec_default.h>

#include <cstdio>

#include "bsp/config.h"
#include "drivers/bus/i2c_bus_lock.h"
#include "utils/gpio_util.h"
#include "utils/time_utils.h"

namespace {
constexpr char   kTag[]       = "audio";
constexpr size_t kWriteChunk  = 512;  // 16 ms@16 kHz，stop 检查粒度
constexpr size_t kMaxFileSize = 2 * 1024 * 1024;

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || static_cast<size_t>(size) > kMaxFileSize) {
        fclose(f);
        return false;
    }
    out.resize(size);
    const size_t n = fread(out.data(), 1, size, f);
    fclose(f);
    return n == static_cast<size_t>(size);
}
}  // namespace

AudioPlayer& AudioPlayer::Get() {
    static AudioPlayer p;
    return p;
}

bool AudioPlayer::Init(i2c_master_bus_handle_t i2c_bus, int volume) {
    if (initialized_)
        return true;
    volume_.store(volume, std::memory_order_relaxed);

    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    esp_log_level_set("ESP_ES_PARSER", ESP_LOG_WARN);

    i2s_chan_config_t chan_cfg   = {};
    chan_cfg.id                  = I2S_NUM_0;
    chan_cfg.role                = I2S_ROLE_MASTER;
    chan_cfg.dma_desc_num        = 6;
    chan_cfg.dma_frame_num       = 240;
    chan_cfg.auto_clear_after_cb = true;  // 没数据时 DMA 填零，避免空闲啸叫
    if (i2s_new_channel(&chan_cfg, &tx_handle_, nullptr) != ESP_OK) {
        ESP_LOGE(kTag, "i2s channel create failed");
        return false;
    }

    i2s_std_config_t std_cfg        = {};
    std_cfg.clk_cfg.sample_rate_hz  = AUDIO_OUTPUT_SAMPLE_RATE;
    std_cfg.clk_cfg.clk_src         = I2S_CLK_SRC_DEFAULT;
    std_cfg.clk_cfg.mclk_multiple   = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_cfg.slot_cfg.slot_mode      = I2S_SLOT_MODE_MONO;
    std_cfg.slot_cfg.slot_mask      = I2S_STD_SLOT_LEFT;
    std_cfg.slot_cfg.ws_width       = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.ws_pol         = false;
    std_cfg.slot_cfg.bit_shift      = true;
    std_cfg.slot_cfg.left_align     = true;
    std_cfg.slot_cfg.big_endian     = false;
    std_cfg.slot_cfg.bit_order_lsb  = false;
    std_cfg.gpio_cfg.mclk           = AUDIO_I2S_GPIO_MCLK;
    std_cfg.gpio_cfg.bclk           = AUDIO_I2S_GPIO_BCLK;
    std_cfg.gpio_cfg.ws             = AUDIO_I2S_GPIO_WS;
    std_cfg.gpio_cfg.dout           = AUDIO_I2S_GPIO_DOUT;
    std_cfg.gpio_cfg.din            = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(tx_handle_, &std_cfg) != ESP_OK) {
        ESP_LOGE(kTag, "i2s tx init failed");
        return false;
    }

    audio_codec_i2s_cfg_t i2s_data_cfg = {};
    i2s_data_cfg.port                  = I2S_NUM_0;
    i2s_data_cfg.tx_handle             = tx_handle_;
    data_if_                           = audio_codec_new_i2s_data(&i2s_data_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port                  = I2C_NUM_0;
    i2c_cfg.addr                  = AUDIO_CODEC_ES8311_ADDR;
    i2c_cfg.bus_handle            = i2c_bus;
    {
        ScopedI2cBusLock lock("AudioPlayer::Init");
        ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    }
    gpio_if_ = audio_codec_new_gpio();
    if (!data_if_ || !ctrl_if_ || !gpio_if_) {
        ESP_LOGE(kTag, "codec interface create failed");
        return false;
    }

    // PA 由这里自己控制：codec 打开、DAC 稳定 100 ms 后再拉高，避免爆音。
    es8311_codec_cfg_t es_cfg        = {};
    es_cfg.ctrl_if                   = ctrl_if_;
    es_cfg.gpio_if                   = gpio_if_;
    es_cfg.codec_mode                = ESP_CODEC_DEV_WORK_MODE_DAC;
    es_cfg.pa_pin                    = -1;
    es_cfg.use_mclk                  = true;
    es_cfg.hw_gain.pa_voltage        = 5.0;
    es_cfg.hw_gain.codec_dac_voltage = 3.3;
    codec_if_                        = es8311_codec_new(&es_cfg);
    if (!codec_if_) {
        ESP_LOGE(kTag, "codec create failed chip=es8311");
        return false;
    }
    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type            = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if            = codec_if_;
    dev_cfg.data_if             = data_if_;
    dev_                        = esp_codec_dev_new(&dev_cfg);
    if (!dev_) {
        ESP_LOGE(kTag, "codec device create failed");
        return false;
    }

    mutex_  = xSemaphoreCreateMutex();
    notify_ = xSemaphoreCreateBinary();
    if (xTaskCreatePinnedToCore(&AudioPlayer::TaskEntry, "audio", 16 * 1024, this, 5, &task_, 1) != pdPASS) {
        ESP_LOGE(kTag, "task create failed");
        return false;
    }
    initialized_ = true;
    return true;
}

uint32_t AudioPlayer::Play(const std::string& path) {
    if (!initialized_)
        return 0;
    std::vector<uint8_t> data;
    if (!ReadFile(path, data)) {
        ESP_LOGW(kTag, "read failed path=%s", path.c_str());
        return 0;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    pending_.swap(data);
    has_pending_   = true;
    pending_token_ = next_token_++;
    if (next_token_ == 0)
        next_token_ = 1;
    const uint32_t token = pending_token_;
    stop_flag_.store(true, std::memory_order_release);
    xSemaphoreGive(mutex_);
    xSemaphoreGive(notify_);
    ESP_LOGD(kTag, "play queued path=%s token=%u", path.c_str(), static_cast<unsigned>(token));
    return token;
}

void AudioPlayer::Stop() {
    if (!initialized_)
        return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    has_pending_ = false;
    pending_.clear();
    stop_flag_.store(true, std::memory_order_release);
    xSemaphoreGive(mutex_);
}

void AudioPlayer::SetVolume(int v) {
    if (v < 0)
        v = 0;
    if (v > 100)
        v = 100;
    volume_.store(v, std::memory_order_relaxed);
    if (codec_rate_ != 0)
        SetCodecVolume(v);
}

void AudioPlayer::SetCodecVolume(int v) {
    ScopedI2cBusLock lock("AudioPlayer::volume");
    if (lock.locked())
        esp_codec_dev_set_out_vol(dev_, v);
}

bool AudioPlayer::EnsureCodecOpen(int sample_rate) {
    if (codec_rate_ == sample_rate)
        return true;
    if (codec_rate_ != 0)
        CloseCodec();
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample             = 16;
    fs.channel                     = 1;
    fs.sample_rate                 = static_cast<uint32_t>(sample_rate);
    {
        ScopedI2cBusLock lock("AudioPlayer::open");
        if (!lock.locked())
            return false;
        // esp_codec_dev 打开时会先 disable 尚未 enable 的 channel，IDF 打一条无害的 ERROR，这里临时压掉。
        const esp_log_level_t level = esp_log_level_get("i2s_common");
        esp_log_level_set("i2s_common", ESP_LOG_NONE);
        const int ret = esp_codec_dev_open(dev_, &fs);
        esp_log_level_set("i2s_common", level);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(kTag, "codec open failed rate=%d", sample_rate);
            return false;
        }
        esp_codec_dev_set_out_vol(dev_, volume_.load(std::memory_order_relaxed));
    }
    codec_rate_ = sample_rate;
    vTaskDelay(pdMS_TO_TICKS(100));
    GpioWriteHold(AUDIO_CODEC_PA_PIN, 1);
    ESP_LOGI(kTag, "codec open rate=%d", sample_rate);
    return true;
}

void AudioPlayer::CloseCodec() {
    GpioWriteHold(AUDIO_CODEC_PA_PIN, 0);
    ScopedI2cBusLock lock("AudioPlayer::close");
    esp_codec_dev_close(dev_);
    codec_rate_ = 0;
}

bool AudioPlayer::WritePcm(const uint8_t* pcm, size_t len, bool& unmuted, bool need_unmute) {
    size_t off = 0;
    while (off < len) {
        if (stop_flag_.load(std::memory_order_acquire))
            return false;
        const size_t n = (len - off) > kWriteChunk ? kWriteChunk : (len - off);
        // esp_codec_dev 的 C API 不是 const-correct，写路径不会修改数据。
        if (esp_codec_dev_write(dev_, const_cast<uint8_t*>(pcm + off), static_cast<int>(n)) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(kTag, "i2s write failed");
            return false;
        }
        off += n;
        // 新音频的第一块写下去后再恢复音量，衔接时不会有“啵”声。
        if (need_unmute && !unmuted) {
            SetCodecVolume(volume_.load(std::memory_order_relaxed));
            unmuted = true;
        }
    }
    return true;
}

bool AudioPlayer::Decode(const std::vector<uint8_t>& ogg) {
    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type                   = ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    esp_audio_simple_dec_handle_t dec = nullptr;
    if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "decoder open failed");
        return false;
    }

    std::vector<uint8_t> out(4096);
    esp_audio_simple_dec_raw_t raw = {};
    raw.buffer                     = const_cast<uint8_t*>(ogg.data());
    raw.len                        = ogg.size();
    raw.eos                        = true;

    bool          completed   = true;
    bool          unmuted     = false;
    bool          need_unmute = false;
    bool          opened      = false;
    size_t        pcm_bytes   = 0;
    const int64_t start_ms    = time_utils::NowMs();
    while (raw.len > 0) {
        if (stop_flag_.load(std::memory_order_acquire)) {
            completed = false;
            break;
        }
        esp_audio_simple_dec_out_t frame = {};
        frame.buffer                     = out.data();
        frame.len                        = out.size();
        const esp_audio_err_t ret        = esp_audio_simple_dec_process(dec, &raw, &frame);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            out.resize(frame.needed_size);
            continue;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(kTag, "decode failed ret=%d", static_cast<int>(ret));
            completed = false;
            break;
        }
        if (raw.consumed == 0 && frame.decoded_size == 0)
            break;
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
        if (frame.decoded_size == 0)
            continue;

        if (!opened) {
            esp_audio_simple_dec_info_t info = {};
            esp_audio_simple_dec_get_info(dec, &info);
            if (info.channel != 1 || info.bits_per_sample != 16)
                ESP_LOGW(kTag, "unexpected format ch=%u bits=%u", info.channel, info.bits_per_sample);
            // 切换音频前先静音：上一段末尾和新段开头的波形跳变会被 PA 放大成“啵”。
            need_unmute = played_once_ && codec_rate_ == static_cast<int>(info.sample_rate);
            if (need_unmute) {
                SetCodecVolume(0);
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (!EnsureCodecOpen(static_cast<int>(info.sample_rate))) {
                completed = false;
                break;
            }
            played_once_ = true;
            opened       = true;
        }
        if (!WritePcm(frame.buffer, frame.decoded_size, unmuted, need_unmute)) {
            completed = false;
            break;
        }
        pcm_bytes += frame.decoded_size;
    }
    if (need_unmute && !unmuted)
        SetCodecVolume(volume_.load(std::memory_order_relaxed));
    esp_audio_simple_dec_close(dec);
    ESP_LOGI(kTag, "play end completed=%d pcm_bytes=%u rate=%d elapsed_ms=%lld", completed ? 1 : 0,
             static_cast<unsigned>(pcm_bytes), codec_rate_, static_cast<long long>(time_utils::NowMs() - start_ms));
    return completed && pcm_bytes > 0;
}

void AudioPlayer::TaskEntry(void* arg) {
    static_cast<AudioPlayer*>(arg)->TaskLoop();
}

void AudioPlayer::TaskLoop() {
    std::vector<uint8_t> data;
    while (true) {
        xSemaphoreTake(notify_, portMAX_DELAY);
        xSemaphoreTake(mutex_, portMAX_DELAY);
        if (!has_pending_) {
            xSemaphoreGive(mutex_);
            continue;
        }
        data.swap(pending_);
        pending_.clear();
        has_pending_         = false;
        const uint32_t token = pending_token_;
        stop_flag_.store(false, std::memory_order_release);
        xSemaphoreGive(mutex_);

        playing_.store(true, std::memory_order_release);
        current_token_.store(token, std::memory_order_release);
        const bool completed = Decode(data);
        playing_.store(false, std::memory_order_release);
        if (completed && on_done_)
            on_done_(token);
    }
}
