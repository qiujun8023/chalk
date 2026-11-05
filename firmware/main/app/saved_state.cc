#include "app/saved_state.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace {
constexpr char kTag[] = "state";
}

bool SavedState::Load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char         buf[1024];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> doc(cJSON_Parse(buf), cJSON_Delete);
    if (!doc) {
        ESP_LOGW(kTag, "parse failed");
        return false;
    }
    const cJSON* v = nullptr;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(doc.get(), "volume")))
        volume = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(doc.get(), "group")))
        group = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(doc.get(), "page")))
        page = v->valueint;
    positions.clear();
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(doc.get(), "positions")) {
        positions.push_back(cJSON_IsNumber(v) ? static_cast<uint16_t>(v->valueint) : 0);
    }
    return true;
}

bool SavedState::Save(const std::string& path) const {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> doc(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddNumberToObject(doc.get(), "volume", volume);
    cJSON_AddNumberToObject(doc.get(), "group", group);
    cJSON_AddNumberToObject(doc.get(), "page", page);
    cJSON* arr = cJSON_AddArrayToObject(doc.get(), "positions");
    for (uint16_t p : positions)
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(p));

    std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(doc.get()), cJSON_free);
    // 先写临时文件再改名，断电不会留下半个文件。
    const std::string tmp = path + ".tmp";
    FILE*             f   = fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    const size_t len = strlen(text.get());
    const bool   ok  = fwrite(text.get(), 1, len, f) == len;
    fclose(f);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        ESP_LOGW(kTag, "save failed");
        return false;
    }
    return true;
}
