#include <esp_littlefs.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app/app.h"
#include "bsp/board.h"
#include "drivers/audio/audio_player.h"
#include "power/sleep.h"

namespace {
constexpr char kTag[] = "main";

void MountContent() {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path               = "/c";
    conf.partition_label         = "storage";
    conf.format_if_mount_failed  = false;
    const esp_err_t err          = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK)
        ESP_LOGE(kTag, "content mount failed err=%s", esp_err_to_name(err));
}

void AppTask(void* arg) {
    static App app;
    app.Run(*static_cast<power::WakeSource*>(arg));
}
}  // namespace

extern "C" void app_main() {
    static power::WakeSource wake = power::DetectWakeSource();
    Board::Get().Init();
    MountContent();
    // 音量在 App 读出保存状态后再设置。
    AudioPlayer::Get().Init(Board::Get().i2c_bus(), 70);
    ESP_LOGI(kTag, "boot wake=%d", static_cast<int>(wake));
    xTaskCreatePinnedToCore(AppTask, "app", 8 * 1024, &wake, 4, nullptr, 0);
}
