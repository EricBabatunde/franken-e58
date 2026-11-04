#include "wifi_ap.h"

#include <atomic>
#include <cstring>

#include "board_config.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "seqlock.h"
#include "wifi_credentials.h"  // git-ignored; see wifi_credentials.example.h

namespace comms::wifi_ap {
namespace {

constexpr size_t kSsidMax = 32;
constexpr size_t kPasswordMin = 8;   // WPA2-PSK passphrase: 8 to 63 characters
constexpr size_t kPasswordMax = 63;
static_assert(sizeof(FE_WIFI_SSID) - 1 >= 1 && sizeof(FE_WIFI_SSID) - 1 <= kSsidMax,
              "FE_WIFI_SSID must be 1 to 32 characters");
static_assert(sizeof(FE_WIFI_PASSWORD) - 1 >= kPasswordMin && sizeof(FE_WIFI_PASSWORD) - 1 <= kPasswordMax,
              "FE_WIFI_PASSWORD must be 8 to 63 characters for WPA2-PSK");

constexpr bool same(const char* a, const char* b) {
    return *a == *b && (*a == '\0' || same(a + 1, b + 1));
}
constexpr bool kPlaceholder = same(FE_WIFI_SSID, "CHANGE-ME-SSID") || same(FE_WIFI_PASSWORD, "CHANGE-ME-PASSWORD");

constexpr uint32_t kStartWaitMs = 2000;

std::atomic<int64_t> s_start_call_us{0};
std::atomic<int64_t> s_ap_start_us{0};
esp_err_t s_err = ESP_OK;               // Core 0 test task only
SeqlockSlot<StationLog> s_log;
StationLog s_log_work;                  // event handler only (single writer)

void on_wifi_event(void*, esp_event_base_t, int32_t id, void* data) {
    if (id == WIFI_EVENT_AP_START) {
        s_ap_start_us.store(esp_timer_get_time(), std::memory_order_release);
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        const auto* e = static_cast<const wifi_event_ap_staconnected_t*>(data);
        s_log_work.joins++;
        s_log_work.last_join = true;
        std::memcpy(s_log_work.last_mac, e->mac, sizeof(s_log_work.last_mac));
        s_log_work.last_reason = 0;
        s_log_work.seq++;
        s_log.write(s_log_work);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const auto* e = static_cast<const wifi_event_ap_stadisconnected_t*>(data);
        s_log_work.leaves++;
        s_log_work.last_join = false;
        std::memcpy(s_log_work.last_mac, e->mac, sizeof(s_log_work.last_mac));
        s_log_work.last_reason = e->reason;
        s_log_work.seq++;
        s_log.write(s_log_work);
    }
}

esp_err_t keep(esp_err_t e) {
    if (s_err == ESP_OK && e != ESP_OK) {
        s_err = e;
    }
    return e;
}

}  // namespace

esp_err_t start() {
    keep(esp_netif_init());
    keep(esp_event_loop_create_default());
    if (esp_netif_create_default_wifi_ap() == nullptr) {
        keep(ESP_FAIL);
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = 0;  // also the default with CONFIG_ESP_WIFI_NVS_ENABLED=n
    if (keep(esp_wifi_init(&init)) != ESP_OK) {
        return s_err;
    }
    keep(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    keep(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr));
    keep(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t cfg = {};
    std::memcpy(cfg.ap.ssid, FE_WIFI_SSID, sizeof(FE_WIFI_SSID) - 1);
    cfg.ap.ssid_len = sizeof(FE_WIFI_SSID) - 1;
    std::memcpy(cfg.ap.password, FE_WIFI_PASSWORD, sizeof(FE_WIFI_PASSWORD) - 1);
    cfg.ap.channel = board::kWifiChannel;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = board::kWifiMaxStations;
    // B-M6-08: both set explicitly. A zeroed wifi_ap_config_t holds DTIM 0; IDF 5.5.3 documents
    // dtim_period 1 to 10, default 1, and beacon_interval 100 to 60000 TU, default 100
    // (esp_wifi_types_generic.h).
    cfg.ap.beacon_interval = board::kWifiBeaconIntervalTu;
    cfg.ap.dtim_period = board::kWifiDtimPeriod;
    cfg.ap.pmf_cfg.required = false;
    keep(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    keep(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    keep(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
    keep(esp_wifi_set_ps(WIFI_PS_NONE));

    s_start_call_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    if (keep(esp_wifi_start()) != ESP_OK) {
        return s_err;
    }
    for (uint32_t waited = 0; waited < kStartWaitMs && s_ap_start_us.load(std::memory_order_acquire) == 0;
         waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_ap_start_us.load(std::memory_order_acquire) == 0) {
        keep(ESP_ERR_TIMEOUT);
    }
    return s_err;
}

Report report() {
    Report r;
    r.err = s_err;
    const int64_t t0 = s_start_call_us.load(std::memory_order_relaxed);
    const int64_t t1 = s_ap_start_us.load(std::memory_order_acquire);
    r.started = t1 != 0;
    r.start_us = r.started ? static_cast<uint32_t>(t1 - t0) : 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&r.channel, &second);
    wifi_config_t cfg = {};
    if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
        r.authmode = cfg.ap.authmode;
        r.max_connection = cfg.ap.max_connection;
        r.beacon_interval = cfg.ap.beacon_interval;
        r.dtim_period = cfg.ap.dtim_period;
    }
    esp_wifi_get_ps(&r.ps);
    esp_wifi_get_bandwidth(WIFI_IF_AP, &r.bandwidth);
    esp_wifi_get_protocol(WIFI_IF_AP, &r.protocol);
    r.placeholder_credentials = kPlaceholder;
    return r;
}

bool station_log(StationLog* out) { return s_log.read(out); }

int stations() {
    wifi_sta_list_t list = {};
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : -1;
}

}  // namespace comms::wifi_ap
