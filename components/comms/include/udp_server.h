#pragma once
#include "esp_err.h"

// UDP control and ping server (blueprint section 4.6, M6). One task, "udp_rx", pinned to
// Core 0 at board::kUdpTaskPriority, listening on board::kUdpPort. One static receive buffer,
// no heap use per packet. Valid pings get a pong to the sender's address; valid control
// frames update the UDP setpoint slot; anything else is counted by cause and dropped
// silently. Counters: comms::link::counters(kUdp).
namespace comms::udp_server {

esp_err_t start();  // call once, after comms::wifi_ap::start()

}  // namespace comms::udp_server
