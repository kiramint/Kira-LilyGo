#include "runtime.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <ctime>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_lvgl_port.h"
#include "esp_vfs_fat.h"
#include "nvs.h"

namespace embed {
namespace {
using Action = std::function<void()>;
struct Row {
    std::string text;
    Action select;
};
enum class Page { home, menu, app, editor, level, busy };
Page page;
std::vector<Row> rows;
int index_;
lv_obj_t *heading, *body;
std::string title, entry;
std::function<void(std::string)> submit;
bool secret, editing_light;
int letter, original_level;
std::atomic<int> storage_job; // 1: mount, 2: format, 3: result available
std::mutex stats_mutex;
struct Stats {
    uint64_t total, free;
    int mv;
    esp_err_t result;
} stats{};
std::string directory = "/sdcard";
void timezone_menu();
void home();
void selector();
void settings();
void files();
void storage();
void bluetooth();
void wireless();

void sensors(void *) {
    for (;;) {
        int job = storage_job.load();
        esp_err_t result = ESP_OK;
        if (job == 1)
            result = mount_card();
        if (job == 2)
            result = format_card();
        uint64_t total = 0, free = 0;
        if (mounted() && esp_vfs_fat_info("/sdcard", &total, &free) != ESP_OK)
            total = free = 0;
        int mv = battery_mv();
        {
            std::lock_guard lock(stats_mutex);
            if (stats.mv > 0 && mv > 0)
                mv = (3 * stats.mv + mv) / 4;
            stats.total = total;
            stats.free = free;
            stats.mv = mv;
            if (job == 1 || job == 2)
                stats.result = result;
        }
        if (job == 1 || job == 2)
            storage_job = 3;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
Stats snapshot() {
    std::lock_guard lock(stats_mutex);
    return stats;
}
void refresh() {
    lv_label_set_text(heading, title.c_str());
    std::string text;
    int start = std::max(0, index_ - 2);
    for (int i = start; i < int(rows.size()) && i < start + 5; ++i)
        text += (i == index_ ? "> " : "  ") + rows[i].text + "\n";
    lv_label_set_text(body, text.c_str());
}
void menu(std::string name, std::vector<Row> options) {
    page = Page::menu;
    title = std::move(name);
    rows = std::move(options);
    index_ = 0;
    refresh();
}
void message(std::string text, Action back) { menu(std::move(text), {{"Back", std::move(back)}}); }
void confirmed(std::string title, Action cancel, Action accept) {
    menu(std::move(title), {{"Cancel", std::move(cancel)}, {"Confirm", std::move(accept)}});
}
void result(int err, Action back) {
    if (!err)
        back();
    else
        message("Error " + std::to_string(err), std::move(back));
}
void update_home() {
    auto s = snapshot();
    char date[40] = "Time not synced";
    auto now = time(nullptr);
    if (now > 1600000000) {
        tm local{};
        localtime_r(&now, &local);
        strftime(date, sizeof(date), "%Y-%m-%d    %H:%M:%S", &local);
    }
    lv_label_set_text(heading, date);
    auto usage = [](uint32_t caps) {
        auto total = heap_caps_get_total_size(caps);
        return std::to_string((total - heap_caps_get_free_size(caps)) / 1024) + "/" +
               std::to_string(total / 1024) + " KiB";
    };
    std::string text =
        "Battery   " + (s.mv < 0 ? std::string("--") : std::to_string(s.mv) + " mV") + "      Background   " +
        std::to_string(background_count()) + "\nRAM       " + usage(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) +
        "\nPSRAM   " + usage(MALLOC_CAP_SPIRAM) + "\nTF           " +
        (s.total ? std::to_string((s.total - s.free) >> 20) + "/" + std::to_string(s.total >> 20) + " MiB"
                 : "Not mounted") +
        "\nClick for apps";
    lv_label_set_text(body, text.c_str());
}
void home() {
    if (page == Page::level)
        set_level(editing_light, original_level, false);
    if (!leave_app()) {
        message("App still stopping", home);
        return;
    }
    ble_leave();
    entry.clear();
    submit = {};
    rows.clear();
    page = Page::home;
    update_home();
}
void selector() {
    std::vector<Row> out{{"Settings", settings}};
    for (auto p : packages()) {
        out.push_back({p.name + (running(p.id) ? "  [running]" : ""), [p] {
                           lv_label_set_text(body, "");
                           lv_label_set_text(heading, "");
                           auto e = launch(p);
                           if (e.empty())
                               page = Page::app;
                           else
                               message(e, selector);
                       }});
    }
    // Stop remains available even when the card has been removed.
    for (auto &id : resident_ids())
        out.push_back({"Stop " + id, [id] {
                           if (stop_app(id))
                               selector();
                           else
                               message("App still busy", selector);
                       }});
    out.push_back({"Home", home});
    menu("Applications", std::move(out));
}
void show_editor() {
    std::string selection = letter == 0   ? "[Done]"
                            : letter == 1 ? "[Delete]"
                                          : std::string("[ ") + char(letter + 30) + " ]";
    lv_label_set_text(heading, title.c_str());
    std::string text = (secret ? std::string(entry.size(), '*') : entry) + "\n\n" + selection;
    lv_label_set_text(body, text.c_str());
}
void edit(std::string label, bool mask, std::function<void(std::string)> done) {
    page = Page::editor;
    title = std::move(label);
    secret = mask;
    submit = std::move(done);
    entry.clear();
    letter = 0;
    show_editor();
}
void wireless() {
    std::vector<Row> out{{"Scan",
                          [] {
                              wifi_scan();
                              message("Scanning Wi-Fi", wireless);
                          }},
                         {"Refresh", wireless},
                         {"Disconnect",
                          [] {
                              wifi_disconnect(false);
                              wireless();
                          }},
                         {"Forget network", [] {
                              wifi_disconnect(true);
                              wireless();
                          }}};
    for (auto &ap : wifi_results())
        out.push_back({ap.ssid + "  " + std::to_string(ap.rssi) + " dBm", [ssid = ap.ssid] {
                           edit("Password / " + ssid, true,
                                [ssid](auto pass) { result(wifi_join(ssid, pass), wireless); });
                       }});
    out.push_back({"Back", settings});
    menu("Wi-Fi  " + wifi_status(), std::move(out));
}
void bluetooth() {
    std::vector<Row> out{
        {"Power on", [] { result(ble_power(true), bluetooth); }},
        {"Scan", [] { result(ble_scan(), bluetooth); }},
        {"Refresh", bluetooth},
        {"Disconnect",
         [] {
             ble_leave();
             bluetooth();
         }},
        {"Clear bonds",
         [] { confirmed("Clear BLE bonds?", bluetooth, [] { result(ble_forget(), bluetooth); }); }},
        {"Power off", [] { result(ble_power(false), bluetooth); }}};
    for (auto &p : ble_results())
        out.push_back(
            {p.name.empty() ? p.address : p.name, [a = p.address] { result(ble_connect_to(a), bluetooth); }});
    out.push_back({"Back", [] {
                       ble_leave();
                       settings();
                   }});
    menu("BLE  " + ble_status(), std::move(out));
}
bool valid_filename(const std::string &s) {
    return !s.empty() && s != "." && s != ".." && s.back() != '.' && s.back() != ' ' &&
           s.find_first_of("/\\:*?\"<>|") == std::string::npos;
}
void file_actions(const std::string &path, bool folder) {
    std::vector<Row> out;
    if (folder)
        out.push_back({"Open", [path] {
                           directory = path;
                           files();
                       }});
    out.push_back({"Rename", [path] {
                       edit("New name", false, [path](auto name) {
                           if (!valid_filename(name)) {
                               message("Invalid name", files);
                               return;
                           }
                           auto dest = directory + "/" + name;
                           struct stat st{};
                           if (!stat(dest.c_str(), &st)) {
                               message("Name exists", files);
                               return;
                           }
                           result(rename(path.c_str(), dest.c_str()) ? errno : 0, files);
                       });
                   }});
    out.push_back({"Delete", [path, folder] {
                       confirmed("Delete " + path.substr(path.rfind('/') + 1) + "?", files, [path, folder] {
                           int rc = folder ? rmdir(path.c_str()) : unlink(path.c_str());
                           result(rc ? errno : 0, files);
                       });
                   }});
    out.push_back({"Back", files});
    menu(path.substr(path.rfind('/') + 1), std::move(out));
}
void files() {
    DIR *dir = opendir(directory.c_str());
    if (!dir) {
        message(strerror(errno), storage);
        return;
    }
    std::vector<Row> out{{"Back",
                          [] {
                              if (directory == "/sdcard")
                                  storage();
                              else {
                                  directory.resize(directory.rfind('/'));
                                  files();
                              }
                          }},
                         {"Create directory", [] {
                              edit("Directory name", false, [](auto s) {
                                  if (!valid_filename(s)) {
                                      message("Invalid name", files);
                                      return;
                                  }
                                  result(mkdir((directory + "/" + s).c_str(), 0777) ? errno : 0, files);
                              });
                          }}};
    while (auto e = readdir(dir)) {
        std::string name(e->d_name);
        if (name == "." || name == "..")
            continue;
        auto path = directory + "/" + name;
        bool folder = e->d_type == DT_DIR;
        out.push_back({name + (folder ? "/" : ""), [path, folder] { file_actions(path, folder); }});
    }
    closedir(dir);
    menu(directory, std::move(out));
}
void run_storage(int job) {
    if (!stop_all()) {
        message("Card busy: app still running", storage);
        return;
    }
    page = Page::busy;
    lv_label_set_text(heading, "TF card");
    lv_label_set_text(body, "Working...");
    storage_job = job;
}
void storage() {
    auto s = snapshot();
    menu(s.total ? "TF  " + std::to_string(s.free >> 20) + "/" + std::to_string(s.total >> 20) + " MiB free"
                 : "TF not mounted",
         {{"Browse files",
           [] {
               directory = "/sdcard";
               files();
           }},
          {"Mount / retry", [] { run_storage(1); }},
          {"Format card",
           [] {
               confirmed("Erase all files on TF card?", storage,
                         [] { confirmed("Confirm TF erase", storage, [] { run_storage(2); }); });
           }},
          {"Back", settings}});
}
void show_level() {
    lv_label_set_text_fmt(body, "%d %%\n\nTurn to adjust / click to save",
                          editing_light ? brightness() : volume());
}
void level(bool light) {
    page = Page::level;
    editing_light = light;
    original_level = light ? brightness() : volume();
    lv_label_set_text(heading, light ? "Brightness" : "Volume");
    show_level();
}
void load_timezone() {
    char tz[64] = "CST-8";
    nvs_handle_t nvs;
    if (nvs_open("system", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(tz);
        nvs_get_str(nvs, "tz", tz, &size);
        nvs_close(nvs);
    }
    setenv("TZ", tz, 1);
    tzset();
}
void timezone_menu() {
    edit("POSIX timezone", false, [](auto tz) {
        if (tz.empty() || tz.size() > 63) {
            message("Invalid timezone", settings);
            return;
        }
        nvs_handle_t nvs;
        auto err = nvs_open("system", NVS_READWRITE, &nvs);
        if (!err) {
            err = nvs_set_str(nvs, "tz", tz.c_str());
            if (!err)
                err = nvs_commit(nvs);
            nvs_close(nvs);
        }
        if (!err)
            load_timezone();
        result(err, settings);
    });
}
void settings() {
    menu("Settings", {{"Wi-Fi", wireless},
                      {"Bluetooth LE", bluetooth},
                      {"Storage", storage},
                      {"Timezone", timezone_menu},
                      {"Volume", [] { level(false); }},
                      {"Brightness", [] { level(true); }},
                      {"Battery",
                       [] {
                           int mv = snapshot().mv;
                           message(mv < 0 ? "ADC unavailable" : std::to_string(mv) + " mV", settings);
                       }},
                      {"Apps", selector}});
}
void handle(Input key) {
    if (page == Page::busy)
        return;
    if (key == Input::home) {
        home();
        return;
    }
    int delta = key == Input::left ? -1 : key == Input::right ? 1 : 0;
    switch (page) {
    case Page::home:
        if (key == Input::click)
            selector();
        break;
    case Page::menu:
        if (rows.empty())
            break;
        if (delta) {
            int n = rows.size();
            index_ = (index_ + delta + n) % n;
            refresh();
        } else {
            auto action = rows[index_].select;
            action();
        }
        break;
    case Page::editor:
        if (delta)
            letter = (letter + delta + 97) % 97;
        else if (!letter) {
            auto action = std::move(submit);
            action(std::move(entry));
            return;
        } else if (letter == 1) {
            if (!entry.empty())
                entry.pop_back();
        } else if (entry.size() < 128)
            entry.push_back(char(letter + 30));
        show_editor();
        break;
    case Page::level:
        if (delta) {
            set_level(editing_light, (editing_light ? brightness() : volume()) + delta * 5, false);
            show_level();
        } else {
            set_level(editing_light, editing_light ? brightness() : volume(), true);
            settings();
        }
        break;
    case Page::app:
        app_input(key);
        break;
    case Page::busy:
        break;
    }
}
} // namespace
void ui_start() {
    load_timezone();
    runtime_init();
    ESP_ERROR_CHECK(
        xTaskCreate(sensors, "board_stats", 4096, nullptr, 3, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    lvgl_port_lock(0);
    auto root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(0x101923), 0);
    heading = lv_label_create(root);
    body = lv_label_create(root);
    for (auto label : {heading, body}) {
        lv_obj_set_width(label, 300);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    }
    lv_obj_set_pos(heading, 10, 7);
    lv_obj_set_pos(body, 10, 35);
    lv_obj_set_style_text_color(heading, lv_color_hex(0x7ce4bd), 0);
    lv_obj_set_style_text_color(body, lv_color_white(), 0);
    home();
    lv_timer_create(
        [](lv_timer_t *) {
            Input key;
            while (read_input(key))
                handle(key);
            tick_apps();
            if (page == Page::busy && storage_job == 3) {
                storage_job = 0;
                result(snapshot().result, storage);
            }
        },
        10, nullptr);
    lv_timer_create(
        [](lv_timer_t *) {
            if (page == Page::home)
                update_home();
        },
        1000, nullptr);
    lvgl_port_unlock();
}
} // namespace embed
