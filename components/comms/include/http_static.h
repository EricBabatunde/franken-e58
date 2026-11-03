#pragma once
#include <cstdint>

#include "esp_err.h"
#include "esp_http_server.h"

// Static file server (esp_http_server) for the LittleFS mount (hal_fs). GET only.
// - "/" serves index.html. Every file is stored gzip-compressed as <name>.gz and sent with
//   Content-Encoding: gzip and the Content-Type of <name>.
// - Paths containing ".." get 400, unknown paths 404, other methods 405 (from httpd).
// - "/version" returns the firmware's compile-time version as JSON (approved M5 deviation).
// - An optional extra handler (the M6 WebSocket "/ws", comms::ws_server) is registered ahead
//   of the "/*" wildcard; without it "/ws" is an unknown path (404).
// Files go out in chunks from one static buffer (board::kHttpChunkBytes): no per-request
// buffer allocation. The server task is pinned to Core 0 and sets TCP_NODELAY on each socket.
namespace comms::http_static {

// Call once, after hal_fs::mount() and comms::wifi_ap::start(). `extra` may be null.
esp_err_t start(const httpd_uri_t* extra = nullptr);

struct Counters {
    uint32_t requests = 0;     // GET requests reaching a handler
    uint32_t ok = 0;           // 200 responses completed
    uint32_t not_found = 0;
    uint32_t bad_request = 0;  // ".." and malformed paths
    uint32_t send_errors = 0;  // client went away mid-response
    uint64_t body_bytes = 0;   // file bytes sent (compressed)
};
Counters counters();

}  // namespace comms::http_static
