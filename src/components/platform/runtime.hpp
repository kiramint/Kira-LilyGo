#pragma once
#include "platform.hpp"
namespace embed {
struct Package {
    std::string id, name, firmware;
    bool background;
};
std::vector<Package> packages();
void runtime_init();
std::string launch(const Package &);
bool leave_app();
bool stop_app(const std::string &id);
bool stop_all();
bool running(const std::string &id);
std::vector<std::string> resident_ids();
unsigned background_count();
void tick_apps();
void app_input(Input);
struct Peripheral {
    std::string name, address;
};
void wifi_scan();
struct AccessPoint {
    std::string ssid;
    int rssi;
};
std::vector<AccessPoint> wifi_results();
std::string wifi_status();
esp_err_t wifi_join(const std::string &, const std::string &);
void wifi_disconnect(bool forget);
int ble_power(bool on);
int ble_scan();
std::vector<Peripheral> ble_results();
int ble_connect_to(const std::string &);
void ble_leave();
int ble_forget();
std::string ble_status();
} // namespace embed
