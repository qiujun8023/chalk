#include "app/library.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstdio>
#include <memory>

namespace library {

namespace {
constexpr char kTag[] = "library";

std::string Str(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

int Int(const cJSON* obj, const char* key, int def) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

bool ParseEntry(const cJSON* j, Entry& e) {
    const std::string type = Str(j, "type");
    e.name                 = Str(j, "name");
    e.title_audio          = Str(j, "titleAudio");
    if (type == "story") {
        e.kind            = Kind::kStory;
        const cJSON* page = nullptr;
        cJSON_ArrayForEach(page, cJSON_GetObjectItemCaseSensitive(j, "pages")) {
            e.pages.push_back({Str(page, "img"), Str(page, "audio")});
        }
        return !e.pages.empty();
    }
    if (type == "figure") {
        e.kind         = Kind::kFigure;
        e.base         = Str(j, "base");
        e.intro        = Str(j, "intro");
        const cJSON* f = nullptr;
        cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(j, "frames")) {
            e.frames.push_back({Str(f, "img"), Int(f, "ms", 0)});
        }
        return !e.base.empty();
    }
    ESP_LOGW(kTag, "unknown entry type=%s name=%s", type.c_str(), e.name.c_str());
    return false;
}
}  // namespace

bool Load(const std::string& root, Library& out) {
    const std::string path = root + "/library.json";
    FILE*             f    = fopen(path.c_str(), "rb");
    if (!f) {
        ESP_LOGE(kTag, "open failed path=%s", path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string text(size > 0 ? size : 0, '\0');
    const size_t n = fread(text.data(), 1, text.size(), f);
    fclose(f);
    if (n != text.size()) {
        ESP_LOGE(kTag, "read failed path=%s", path.c_str());
        return false;
    }

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> doc(cJSON_Parse(text.c_str()), cJSON_Delete);
    if (!doc) {
        ESP_LOGE(kTag, "parse failed path=%s", path.c_str());
        return false;
    }
    out.volume_sound = Str(doc.get(), "volumeSound");
    out.font16       = Str(doc.get(), "font16");
    out.font12       = Str(doc.get(), "font12");
    const cJSON* g   = nullptr;
    cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(doc.get(), "groups")) {
        Group        group{Str(g, "name"), Str(g, "titleAudio"), {}};
        const cJSON* j = nullptr;
        cJSON_ArrayForEach(j, cJSON_GetObjectItemCaseSensitive(g, "entries")) {
            Entry e;
            if (ParseEntry(j, e))
                group.entries.push_back(std::move(e));
        }
        if (!group.entries.empty())
            out.groups.push_back(std::move(group));
    }
    ESP_LOGI(kTag, "loaded groups=%u", static_cast<unsigned>(out.groups.size()));
    return !out.groups.empty();
}

}  // namespace library
