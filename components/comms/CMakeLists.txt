# The Wi-Fi credentials live only in a git-ignored header (CLAUDE.md). Fail early and clearly
# if it is missing; the message never shows any credential.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/include/wifi_credentials.h")
    message(FATAL_ERROR
        "components/comms/include/wifi_credentials.h is missing. Copy "
        "components/comms/include/wifi_credentials.example.h to wifi_credentials.h in the same "
        "folder (it is git-ignored) and set your own SSID and password there.")
endif()

# The control frame vectors are embedded for the boot self-test (M6); the same file feeds the
# host unit test, tools/control_frame.py and web/latency.html.
idf_component_register(
    SRCS "wifi_ap.cpp" "http_static.cpp" "control_frame.cpp" "link.cpp" "udp_server.cpp" "ws_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_config esp_wifi esp_netif esp_event esp_http_server seqlock
    PRIV_REQUIRES esp_timer esp_app_format lwip esp_hw_support freertos
    EMBED_TXTFILES "test_vectors/control_frame_vectors.txt")
