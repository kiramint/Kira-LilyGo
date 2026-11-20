#include "runtime.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <memory>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif_sntp.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
extern "C" void ble_store_config_init(void);

namespace embed {
namespace {
std::mutex state;
std::vector<AccessPoint> networks;
std::string station = "Offline", radio = "Off";
std::atomic<bool> reconnect{false};
unsigned retries;
bool powered;
std::atomic<bool> ready, session;
std::atomic<uint16_t> connection{BLE_HS_CONN_HANDLE_NONE};
uint8_t address_type;
struct Peer {
    Peripheral info;
    ble_addr_t addr;
};
std::vector<Peer> peers;
void wifi_event(void *, esp_event_base_t base, int32_t id, void *event) {
    std::lock_guard guard(state);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        char ip[20];
        auto &addr = static_cast<ip_event_got_ip_t *>(event)->ip_info.ip;
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&addr));
        station = ip;
        retries = 0;
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        station = "Disconnected (" +
                  std::to_string(static_cast<wifi_event_sta_disconnected_t *>(event)->reason) + ")";
        if (reconnect && retries++ < 4)
            esp_wifi_connect();
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        uint16_t count = 20;
        auto records = std::make_unique<wifi_ap_record_t[]>(count);
        networks.clear();
        if (esp_wifi_scan_get_ap_records(&count, records.get()) == ESP_OK) {
            for (unsigned i = 0; i < count; ++i) {
                auto ssid = reinterpret_cast<char *>(records[i].ssid);
                if (*ssid &&
                    std::none_of(networks.begin(), networks.end(), [&](auto &ap) { return ap.ssid == ssid; }))
                    networks.push_back({ssid, records[i].rssi});
            }
        }
    }
}
int gap(ble_gap_event *event, void *) {
    std::lock_guard guard(state);
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (!session || peers.size() >= 32)
            break;
        auto &d = event->disc;
        for (auto &p : peers)
            if (!ble_addr_cmp(&p.addr, &d.addr))
                return 0;
        auto a = d.addr.val;
        char addr[18];
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
        ble_hs_adv_fields fields{};
        std::string name;
        if (!ble_hs_adv_parse_fields(&fields, d.data, d.length_data) && fields.name)
            name.assign(reinterpret_cast<const char *>(fields.name), fields.name_len);
        peers.push_back({{name, addr}, d.addr});
        break;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (!event->connect.status) {
            connection = event->connect.conn_handle;
            if (!session)
                ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
            else {
                radio = "Connected";
                ble_gap_security_initiate(connection);
            }
        } else
            radio = "Connection failed";
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        connection = BLE_HS_CONN_HANDLE_NONE;
        radio = "Idle";
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        radio = "Scan complete";
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    default:
        break;
    }
    return 0;
}
void sync_host() {
    ready = !ble_hs_util_ensure_addr(0) && !ble_hs_id_infer_auto(0, &address_type);
    std::lock_guard guard(state);
    radio = ready ? "Idle" : "Address failed";
}
} // namespace
void network_init() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_config_t saved{};
    if (!esp_wifi_get_config(WIFI_IF_STA, &saved) && saved.sta.ssid[0]) {
        reconnect = true;
        esp_wifi_connect();
    }
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_sntp_init(&sntp));
}
void wifi_scan() {
    auto e = esp_wifi_scan_start(nullptr, false);
    if (e) {
        std::lock_guard guard(state);
        station = esp_err_to_name(e);
    }
}
std::vector<AccessPoint> wifi_results() {
    std::lock_guard guard(state);
    return networks;
}
std::string wifi_status() {
    std::lock_guard guard(state);
    return station;
}
esp_err_t wifi_join(const std::string &ssid, const std::string &pass) {
    wifi_config_t cfg{};
    if (ssid.empty() || ssid.size() > sizeof(cfg.sta.ssid) || pass.size() > sizeof(cfg.sta.password))
        return ESP_ERR_INVALID_ARG;
    memcpy(cfg.sta.ssid, ssid.data(), ssid.size());
    memcpy(cfg.sta.password, pass.data(), pass.size());
    wifi_disconnect(false);
    auto err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err)
        return err;
    {
        std::lock_guard guard(state);
        retries = 0;
        station = "Connecting";
    }
    reconnect = true;
    return esp_wifi_connect();
}
void wifi_disconnect(bool forget) {
    reconnect = false;
    esp_wifi_disconnect();
    if (forget) {
        wifi_config_t empty{};
        esp_wifi_set_config(WIFI_IF_STA, &empty);
    }
}
int ble_power(bool on) {
    if (powered == on)
        return 0;
    if (!on) {
        ble_leave();
        int err = nimble_port_stop();
        if (err)
            return err;
        err = nimble_port_deinit();
        if (err)
            return err;
        ready = powered = false;
        std::lock_guard guard(state);
        radio = "Off";
        return 0;
    }
    int err = nimble_port_init();
    if (err)
        return err;
    powered = true;
    ble_hs_cfg.sync_cb = sync_host;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = ble_hs_cfg.sm_their_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    nimble_port_freertos_init([](void *) {
        nimble_port_run();
        nimble_port_freertos_deinit();
    });
    return 0;
}
int ble_scan() {
    if (!ready)
        return BLE_HS_ENOTSYNCED;
    if (ble_gap_disc_active())
        return 0;
    {
        std::lock_guard guard(state);
        peers.clear();
        radio = "Scanning";
    }
    session = true;
    ble_gap_disc_params params{};
    params.filter_duplicates = 1;
    return ble_gap_disc(address_type, 5000, &params, gap, nullptr);
}
std::vector<Peripheral> ble_results() {
    std::lock_guard guard(state);
    std::vector<Peripheral> out;
    for (auto &p : peers)
        out.push_back(p.info);
    return out;
}
int ble_connect_to(const std::string &addr) {
    if (!ready)
        return BLE_HS_ENOTSYNCED;
    ble_addr_t destination{};
    {
        std::lock_guard guard(state);
        auto p =
            std::find_if(peers.begin(), peers.end(), [&](auto &peer) { return peer.info.address == addr; });
        if (p == peers.end())
            return BLE_HS_ENOENT;
        destination = p->addr;
    }
    session = true;
    if (ble_gap_disc_active())
        ble_gap_disc_cancel();
    return ble_gap_connect(address_type, &destination, 10000, nullptr, gap, nullptr);
}
void ble_leave() {
    session = false;
    if (!ready)
        return;
    if (ble_gap_disc_active())
        ble_gap_disc_cancel();
    if (ble_gap_conn_active())
        ble_gap_conn_cancel();
    if (connection != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
}
int ble_forget() { return ready ? ble_store_clear() : BLE_HS_ENOTSYNCED; }
std::string ble_status() {
    std::lock_guard guard(state);
    return radio;
}
} // namespace embed
