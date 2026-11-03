#include "udp_server.h"

#include "board_config.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "lwip/sockets.h"

namespace comms::udp_server {
namespace {

constexpr uint32_t kStackBytes = 3072;
constexpr size_t kRxBytes = 32;  // larger than a frame, so an over-long datagram is seen as one

int s_sock = -1;
TaskHandle_t s_task = nullptr;
uint8_t s_rx[kRxBytes];
uint8_t s_tx[frame::kBytes];
link::Counters s_c;  // udp_rx task only

void rx_task(void*) {
    for (;;) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(s_sock, s_rx, sizeof(s_rx), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        const int64_t t_rx = esp_timer_get_time();
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));  // socket error: back off, keep serving
            continue;
        }
        frame::Frame ping;
        if (link::handle(setpoint::kUdp, s_rx, static_cast<size_t>(n), t_rx, s_c, &ping) == link::Result::kPing) {
            link::build_pong(ping, t_rx, s_c, s_tx);
            if (sendto(s_sock, s_tx, sizeof(s_tx), 0, reinterpret_cast<sockaddr*>(&from), from_len) ==
                static_cast<int>(sizeof(s_tx))) {
                s_c.tx++;
            } else {
                s_c.send_errors++;
            }
        }
        link::counters(setpoint::kUdp).write(s_c);
    }
}

}  // namespace

esp_err_t start() {
    if (s_task != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        return ESP_FAIL;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(board::kUdpPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    link::counters(setpoint::kUdp).write(s_c);
    if (xTaskCreatePinnedToCore(rx_task, "udp_rx", kStackBytes, nullptr, board::kUdpTaskPriority, &s_task, 0) !=
        pdPASS) {
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

}  // namespace comms::udp_server
