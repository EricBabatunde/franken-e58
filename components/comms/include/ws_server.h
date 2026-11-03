#pragma once
#include "esp_http_server.h"

// WebSocket control and ping endpoint "/ws" (blueprint section 4.6, M6), on the M5 HTTP
// server (CONFIG_HTTPD_WS_SUPPORT). The handler runs in the httpd task, pinned to Core 0.
// - Binary frames of exactly 14 bytes only, validated like UDP. Pings get a pong on the same
//   socket; control frames update the WebSocket setpoint slot.
// - Any other frame (text, fragmented, wrong length, failed validation) closes the session.
// - A plain GET to /ws without the WebSocket upgrade gets 400.
// - One static receive buffer: the httpd task runs one handler at a time.
// Counters: comms::link::counters(kWs).
namespace comms::ws_server {

// The "/ws" URI handler, for comms::http_static::start(), which registers it ahead of the
// "/*" wildcard.
const httpd_uri_t* uri();

}  // namespace comms::ws_server
