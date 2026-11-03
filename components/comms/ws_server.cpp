#include "ws_server.h"

#include "esp_timer.h"
#include "link.h"

namespace comms::ws_server {
namespace {

uint8_t s_rx[frame::kBytes];
uint8_t s_tx[frame::kBytes];
link::Counters s_c;  // httpd task only

void publish() { link::counters(setpoint::kWs).write(s_c); }

// Returning an error makes httpd close the session (httpd_sess.c).
esp_err_t close_bad() {
    s_c.closed_bad++;
    publish();
    return ESP_FAIL;
}

esp_err_t on_ws(httpd_req_t* req) {
    const int64_t t_rx = esp_timer_get_time();  // the frame header byte has just been read
    if (req->method == HTTP_GET) {
        // Called once after a completed handshake, and also for a plain GET without the
        // upgrade (httpd_uri.c performs the handshake only when it detected one).
        if (httpd_ws_get_fd_info(req->handle, httpd_req_to_sockfd(req)) != HTTPD_WS_CLIENT_WEBSOCKET) {
            s_c.plain_get++;
            publish();
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "WebSocket upgrade required");
        }
        s_c.sessions++;
        publish();
        return ESP_OK;
    }

    httpd_ws_frame_t f = {};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) {  // length and type only
        return close_bad();
    }
    if (f.type != HTTPD_WS_TYPE_BINARY || !f.final || f.len != frame::kBytes) {
        s_c.rx++;
        if (f.type == HTTPD_WS_TYPE_BINARY && f.final) {
            s_c.err_length++;
        } else {
            s_c.err_type++;  // text, continuation or fragmented frame
        }
        return close_bad();
    }
    f.payload = s_rx;
    if (httpd_ws_recv_frame(req, &f, sizeof(s_rx)) != ESP_OK) {
        return close_bad();
    }

    frame::Frame ping;
    const link::Result r = link::handle(setpoint::kWs, s_rx, f.len, t_rx, s_c, &ping);
    if (r == link::Result::kInvalid) {
        return close_bad();
    }
    esp_err_t e = ESP_OK;
    if (r == link::Result::kPing) {
        link::build_pong(ping, t_rx, s_c, s_tx);
        httpd_ws_frame_t out = {};
        out.final = true;
        out.type = HTTPD_WS_TYPE_BINARY;
        out.payload = s_tx;
        out.len = sizeof(s_tx);
        e = httpd_ws_send_frame(req, &out);  // synchronous, in the httpd task
        if (e == ESP_OK) {
            s_c.tx++;
        } else {
            s_c.send_errors++;
        }
    }
    publish();
    return e;
}

const httpd_uri_t s_uri = [] {
    httpd_uri_t u = {};
    u.uri = "/ws";
    u.method = HTTP_GET;
    u.handler = on_ws;
    u.user_ctx = nullptr;
    u.is_websocket = true;
    u.handle_ws_control_frames = false;  // httpd answers WebSocket PING and CLOSE itself
    u.supported_subprotocol = nullptr;
    return u;
}();

}  // namespace

const httpd_uri_t* uri() { return &s_uri; }

}  // namespace comms::ws_server
