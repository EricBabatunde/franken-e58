#include "http_static.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "board_config.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "lwip/sockets.h"

namespace comms::http_static {
namespace {

constexpr size_t kMaxPath = 64;    // longest file path accepted after "/fs"
constexpr const char* kGzSuffix = ".gz";

// One chunk buffer for every response. Safe without a lock: esp_http_server runs all
// handlers in its single server task.
alignas(4) char s_chunk[board::kHttpChunkBytes];
char s_version_json[256];

httpd_handle_t s_server = nullptr;
std::atomic<uint32_t> s_requests{0};
std::atomic<uint32_t> s_ok{0};
std::atomic<uint32_t> s_not_found{0};
std::atomic<uint32_t> s_bad_request{0};
std::atomic<uint32_t> s_send_errors{0};
std::atomic<uint64_t> s_body_bytes{0};

const char* content_type(const char* path) {
    const char* dot = std::strrchr(path, '.');
    if (dot == nullptr) return "application/octet-stream";
    if (std::strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (std::strcmp(dot, ".js") == 0) return "application/javascript";
    if (std::strcmp(dot, ".css") == 0) return "text/css";
    if (std::strcmp(dot, ".json") == 0) return "application/json";
    if (std::strcmp(dot, ".svg") == 0) return "image/svg+xml";
    if (std::strcmp(dot, ".ico") == 0) return "image/x-icon";
    if (std::strcmp(dot, ".txt") == 0) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

esp_err_t not_found(httpd_req_t* req) {
    s_not_found.fetch_add(1, std::memory_order_relaxed);
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

esp_err_t on_version(httpd_req_t* req) {
    s_requests.fetch_add(1, std::memory_order_relaxed);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t e = httpd_resp_send(req, s_version_json, HTTPD_RESP_USE_STRLEN);
    if (e == ESP_OK) {
        s_ok.fetch_add(1, std::memory_order_relaxed);
    }
    return e;
}

esp_err_t on_file(httpd_req_t* req) {
    s_requests.fetch_add(1, std::memory_order_relaxed);
    const char* uri = req->uri;
    const size_t n = std::strcspn(uri, "?#");  // the path part only
    if (n == 0 || uri[0] != '/' || n > kMaxPath || std::strstr(uri, "..") != nullptr) {
        s_bad_request.fetch_add(1, std::memory_order_relaxed);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
    }
    char rel[kMaxPath + 1];
    std::memcpy(rel, uri, n);
    rel[n] = '\0';
    char path[sizeof(rel) + 16];
    std::snprintf(path, sizeof(path), "%s%s%s", board::kFsBasePath,
                  (n == 1) ? "/index.html" : rel, kGzSuffix);

    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return not_found(req);
    }
    httpd_resp_set_type(req, content_type((n == 1) ? "/index.html" : rel));
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t e = ESP_OK;
    for (;;) {
        const ssize_t got = read(fd, s_chunk, sizeof(s_chunk));
        if (got <= 0) {
            break;
        }
        e = httpd_resp_send_chunk(req, s_chunk, got);
        if (e != ESP_OK) {
            break;
        }
        s_body_bytes.fetch_add(static_cast<uint64_t>(got), std::memory_order_relaxed);
    }
    close(fd);
    if (e != ESP_OK) {
        s_send_errors.fetch_add(1, std::memory_order_relaxed);
        return ESP_FAIL;  // httpd closes the socket
    }
    e = httpd_resp_send_chunk(req, nullptr, 0);
    if (e == ESP_OK) {
        s_ok.fetch_add(1, std::memory_order_relaxed);
    } else {
        s_send_errors.fetch_add(1, std::memory_order_relaxed);
    }
    return e;
}

// Blueprint section 4.6: TCP_NODELAY on every socket.
esp_err_t on_open(httpd_handle_t, int sockfd) {
    const int one = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return ESP_OK;
}

}  // namespace

esp_err_t start(const httpd_uri_t* extra) {
    if (s_server != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_app_desc_t* app = esp_app_get_description();
    std::snprintf(s_version_json, sizeof(s_version_json),
                  "{\"project\":\"%s\",\"version\":\"%s\",\"date\":\"%s\",\"time\":\"%s\",\"idf\":\"%s\"}",
                  app->project_name, app->version, app->date, app->time, esp_get_idf_version());

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 0;  // Core 0 (blueprint section 4.2); the default is tskNO_AFFINITY
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 4;  // "/version", the optional extra ("/ws" in M6), "/*"
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.open_fn = on_open;
    esp_err_t e = httpd_start(&s_server, &cfg);
    if (e != ESP_OK) {
        return e;
    }
    // Registration order matters: "/version" and the extra handler before the "/*" wildcard.
    // Field by field: with CONFIG_HTTPD_WS_SUPPORT the struct has more members (zeroed here).
    // httpd_register_uri_handler() copies the struct.
    httpd_uri_t version = {};
    version.uri = "/version";
    version.method = HTTP_GET;
    version.handler = on_version;
    httpd_uri_t files = {};
    files.uri = "/*";
    files.method = HTTP_GET;
    files.handler = on_file;
    e = httpd_register_uri_handler(s_server, &version);
    if (e == ESP_OK && extra != nullptr) {
        e = httpd_register_uri_handler(s_server, extra);
    }
    if (e == ESP_OK) {
        e = httpd_register_uri_handler(s_server, &files);
    }
    return e;
}

Counters counters() {
    Counters c;
    c.requests = s_requests.load(std::memory_order_relaxed);
    c.ok = s_ok.load(std::memory_order_relaxed);
    c.not_found = s_not_found.load(std::memory_order_relaxed);
    c.bad_request = s_bad_request.load(std::memory_order_relaxed);
    c.send_errors = s_send_errors.load(std::memory_order_relaxed);
    c.body_bytes = s_body_bytes.load(std::memory_order_relaxed);
    return c;
}

}  // namespace comms::http_static
