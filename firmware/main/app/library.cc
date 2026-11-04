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

bool ParseItem(const cJSON* j, Item& item) {
    const std::string type = Str(j, "type");
    item.id                = Str(j, "id");
    item.name              = Str(j, "name");
    item.title             = Str(j, "title");
    if (type == "story") {
        item.type          = Type::kStory;
        const cJSON* page  = nullptr;
        cJSON_ArrayForEach(page, cJSON_GetObjectItemCaseSensitive(j, "pages")) {
            item.pages.push_back({Str(page, "img"), Str(page, "audio")});
        }
        return !item.pages.empty();
    }
    if (type == "vehicles") {
        item.type      = Type::kVehicles;
        const cJSON* v = nullptr;
        cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(j, "vehicles")) {
            Vehicle vehicle{Str(v, "name"), Str(v, "base"), Str(v, "intro"), {}};
            const cJSON* a = nullptr;
            cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(v, "actions")) {
                Action       action;
                const cJSON* f = nullptr;
                cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(a, "frames")) {
                    action.frames.push_back({Str(f, "img"), Int(f, "ms", 0)});
                }
                vehicle.actions.push_back(std::move(action));
            }
            item.vehicles.push_back(std::move(vehicle));
        }
        return !item.vehicles.empty();
    }
    ESP_LOGW(kTag, "unknown item type=%s id=%s", type.c_str(), item.id.c_str());
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
    out.built        = Str(doc.get(), "built");
    const cJSON* j   = nullptr;
    cJSON_ArrayForEach(j, cJSON_GetObjectItemCaseSensitive(doc.get(), "items")) {
        Item item;
        if (ParseItem(j, item))
            out.items.push_back(std::move(item));
    }
    ESP_LOGI(kTag, "loaded items=%u", static_cast<unsigned>(out.items.size()));
    return !out.items.empty();
}

}  // namespace library
