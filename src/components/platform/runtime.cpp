#include "runtime.hpp"
#include <algorithm>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

extern "C" const esp_elf_symbol_table_t app_exports[];
namespace embed {
namespace {
enum class StopState { live, pending, refused, complete };
struct Instance {
    App app{};
    esp_elf_t elf{};
    Package package;
    std::atomic<StopState> stop{StopState::live};
};
std::vector<std::unique_ptr<Instance>> instances;
Instance *active;
lv_obj_t *canvas;
uint16_t *pixels;
bool valid_id(const std::string &id) {
    return !id.empty() && id.size() <= 48 &&
           id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
               std::string::npos;
}
std::string manifest_string(cJSON *json, const char *key) {
    auto s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, key));
    return s ? s : "";
}
bool compatible(const Package &pkg) {
    auto digest = esp_app_get_description()->app_elf_sha256;
    char hex[65]{};
    for (int i = 0; i < 32; ++i)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return pkg.firmware == hex;
}
std::string load(Instance &instance) {
    if (!compatible(instance.package))
        return "Firmware mismatch";
    auto path = "/sdcard/apps/" + instance.package.id + "/app.elf";
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "rb"), fclose);
    if (!file || fseek(file.get(), 0, SEEK_END))
        return "Cannot open ELF";
    long bytes = ftell(file.get());
    if (bytes <= 0 || bytes > 8 * 1024 * 1024)
        return "Invalid ELF size";
    struct Source {
        FILE *file;
        size_t len;
    } source{file.get(), static_cast<size_t>(bytes)};
    elf_source_t io{};
    io.ctx = &source;
    io.size = [](void *p) { return static_cast<Source *>(p)->len; };
    io.read = [](void *p, size_t offset, void *buf, size_t size) {
        auto f = static_cast<Source *>(p)->file;
        return !fseek(f, offset, SEEK_SET) && fread(buf, 1, size, f) == size ? 0 : -EIO;
    };
    int err = esp_elf_init(&instance.elf);
    if (!err)
        err = esp_elf_relocate_source(&instance.elf, &io);
    if (!err) {
        char *args[] = {reinterpret_cast<char *>(&instance.app), nullptr};
        err = esp_elf_request(&instance.elf, 0, 1, args);
    }
    if (err) {
        esp_elf_deinit(&instance.elf);
        return "ELF load failed " + std::to_string(err);
    }
    ESP_LOGI("apps", "%s at 0x%lx, free PSRAM %u", instance.package.id.c_str(),
             (unsigned long)esp_elf_runtime_base(&instance.elf),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return {};
}
void hide() {
    if (active->app.background)
        active->app.background(active->app);
    lv_group_set_default(nullptr);
    lv_obj_delete(active->app.root);
    lv_group_delete(active->app.focus);
    active->app.focus = nullptr;
    active->app.root = nullptr;
    canvas = nullptr;
    free(pixels);
    pixels = nullptr;
    release_pins();
    ble_leave();
    active = nullptr;
}
} // namespace
void runtime_init() { ESP_ERROR_CHECK(esp_elf_register_symbol(app_exports)); }
std::vector<Package> packages() {
    std::vector<Package> items;
    DIR *d = opendir("/sdcard/apps");
    if (!d)
        return items;
    while (auto e = readdir(d)) {
        std::string id(e->d_name);
        if (!valid_id(id))
            continue;
        std::string path = "/sdcard/apps/" + id + "/manifest.json";
        FILE *f = fopen(path.c_str(), "rb");
        if (!f)
            continue;
        char data[1024];
        size_t n = fread(data, 1, sizeof(data), f);
        fclose(f);
        if (n == sizeof(data))
            continue;
        data[n] = 0;
        auto j = cJSON_Parse(data);
        if (!j)
            continue;
        Package p{manifest_string(j, "id"), manifest_string(j, "name"), manifest_string(j, "firmware"),
                  bool(cJSON_IsTrue(cJSON_GetObjectItem(j, "background")))};
        if (p.id == id && !p.name.empty() && manifest_string(j, "target") == "esp32s3")
            items.push_back(p);
        cJSON_Delete(j);
    }
    closedir(d);
    std::sort(items.begin(), items.end(), [](auto &a, auto &b) { return a.id < b.id; });
    return items;
}
std::string launch(const Package &p) {
    if (active || !valid_id(p.id))
        return "Foreground busy";
    for (auto &i : instances)
        if (i->package.id == p.id) {
            if (i->stop != StopState::live)
                return "Finish stopping this app first";
            active = i.get();
        }
    if (!active) {
        std::unique_ptr<Instance> instance(new (std::nothrow) Instance);
        if (!instance)
            return "Insufficient RAM";
        instance->package = p;
        auto error = load(*instance);
        if (!error.empty())
            return error;
        active = instance.get();
        instances.push_back(std::move(instance));
    }
    auto &a = active->app;
    a.root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(a.root);
    lv_obj_set_size(a.root, 320, 170);
    lv_obj_set_scrollable(a.root, false);
    a.focus = lv_group_create();
    lv_group_set_default(a.focus);
    if (a.foreground)
        a.foreground(a);
    return {};
}
bool stop_app(const std::string &id) {
    auto i = std::find_if(instances.begin(), instances.end(), [&](auto &p) { return p->package.id == id; });
    if (i == instances.end())
        return true;
    auto &inst = **i;
    if (active == &inst)
        hide();
    if (inst.app.stop && inst.stop != StopState::complete) {
        if (inst.stop != StopState::pending) {
            inst.stop = StopState::pending;
            // The host task remains in firmware after the callback returns. It
            // publishes completion only after it has left the module's code.
            if (xTaskCreate(
                    [](void *arg) {
                        auto *i = static_cast<Instance *>(arg);
                        bool done = i->app.stop(i->app);
                        i->stop.store(done ? StopState::complete : StopState::refused);
                        vTaskDelete(nullptr);
                    },
                    "app_stop", 4096, &inst, 4, nullptr) != pdPASS)
                inst.stop = StopState::refused;
        }
        auto since = xTaskGetTickCount();
        while (inst.stop == StopState::pending && xTaskGetTickCount() - since < pdMS_TO_TICKS(100))
            vTaskDelay(1);
        if (inst.stop != StopState::complete)
            return false;
    }
    audio_release(inst.app);
    esp_elf_deinit(&inst.elf);
    instances.erase(i);
    return true;
}
bool leave_app() {
    if (!active)
        return true;
    auto p = active->package;
    hide();
    return p.background || stop_app(p.id);
}
bool stop_all() {
    while (!instances.empty())
        if (!stop_app(instances.back()->package.id))
            return false;
    return true;
}
bool running(const std::string &id) {
    return std::any_of(instances.begin(), instances.end(), [&](auto &p) { return p->package.id == id; });
}
std::vector<std::string> resident_ids() {
    std::vector<std::string> ids;
    for (auto &i : instances)
        ids.push_back(i->package.id);
    return ids;
}
unsigned background_count() { return instances.size() - (active ? 1 : 0); }
void tick_apps() {
    for (auto &i : instances)
        if (i->stop == StopState::live && i->app.tick)
            i->app.tick(i->app);
}
void app_input(Input e) {
    if (!active)
        return;
    auto &a = active->app;
    if (a.input)
        a.input(a, e);
    else if (e == Input::right)
        lv_group_focus_next(a.focus);
    else if (e == Input::left)
        lv_group_focus_prev(a.focus);
    else if (e == Input::click)
        lv_group_send_data(a.focus, LV_KEY_ENTER);
}
bool lamp(App &a, uint8_t r, uint8_t g, uint8_t b) {
    if (!active || &active->app != &a)
        return false;
    write_lamp(r, g, b);
    return true;
}
bool present(App &a, int x, int y, int w, int h, const uint16_t *src, int stride) {
    if (!active || &active->app != &a || !src || w <= 0 || h <= 0 || x < 0 || y < 0 || w > 320 - x ||
        h > 170 - y || stride < w)
        return false;
    if (!canvas) {
        pixels = static_cast<uint16_t *>(heap_caps_calloc(320 * 170, 2, MALLOC_CAP_SPIRAM));
        if (!pixels)
            return false;
        canvas = lv_canvas_create(a.root);
        lv_canvas_set_buffer(canvas, pixels, 320, 170, LV_COLOR_FORMAT_RGB565);
        lv_obj_move_to_index(canvas, 0);
    }
    for (int row = 0; row < h; ++row)
        memcpy(pixels + (y + row) * 320 + x, src + size_t(row) * stride, w * 2);
    lv_area_t area{x, y, x + w - 1, y + h - 1};
    lv_obj_invalidate_area(canvas, &area);
    return true;
}
} // namespace embed
