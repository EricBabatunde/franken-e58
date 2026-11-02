#pragma once
#include <cstdint>

#include "esp_err.h"
#include "esp_wifi_types.h"

// Wi-Fi soft-AP (blueprint section 4.6): fixed channel (board_config), WPA2-PSK, one station,
// HT20, power save off, 802.11 b/g/n, 192.168.4.1 with the default DHCP server.
// Config in RAM only (WIFI_STORAGE_RAM, CONFIG_ESP_WIFI_NVS_ENABLED=n): no flash writes, and
// NVS is never initialised. Core 0 only. Logs nothing that identifies the network: the SSID
// and password are never printed.
namespace comms::wifi_ap {

esp_err_t start();  // netif, event loop, Wi-Fi init and start; waits up to 2 s for AP_START

// What the driver reports back after start(), read through the IDF getters.
struct Report {
    esp_err_t err = ESP_OK;         // first error in start()
    bool started = false;           // AP_START event seen
    uint32_t start_us = 0;          // esp_wifi_start() to the AP_START event
    uint8_t channel = 0;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
    uint8_t max_connection = 0;
    wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
    wifi_bandwidth_t bandwidth = WIFI_BW_HT40;
    uint8_t protocol = 0;           // WIFI_PROTOCOL_11B | 11G | 11N bitmap
    bool placeholder_credentials = false;  // the header still holds the template values
};
Report report();

// Station join and leave events, written by the event handler (sys_evt task, Core 0).
struct StationLog {
    uint32_t seq = 0;          // increments with every event
    uint32_t joins = 0;
    uint32_t leaves = 0;
    bool last_join = false;    // last event was a join (else a leave)
    uint8_t last_mac[6] = {};
    uint16_t last_reason = 0;  // leave reason (wifi_err_reason_t)
};
bool station_log(StationLog* out);
int stations();  // stations associated now

}  // namespace comms::wifi_ap
