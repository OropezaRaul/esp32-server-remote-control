# MicroLink source files are inlined into the main component because
# ESP-IDF v5.3 component discovery does not auto-pickip the nested
# components/microlink/components/microlink directory (only wireguard_lwip
# is found via EXTRA_COMPONENT_DIRS). Inlining is equivalent to a private
# fork for our purposes; updates to microlink upstream need to be merged
# manually by re-running the clone and refreshing the path below.
set(MICROLINK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../components/microlink/components/microlink")

idf_component_register(
    SRCS
        "main.c"
        "system_health.c"
        "wifi.c"
        "wol.c"
        "wake.c"
        "wake_logic.c"
        "ready_probe.c"
        "ready_logic.c"
        "server_client.c"
        "state.c"
        "telegram.c"
        "web_server.c"
        "microlink_wrapper.c"
        "microlink_watchdog.c"
        "wd_logic.c"
        "automation.c"
        "scheduler.c"
        "power_off.c"
        "power_off_logic.c"
        "power_event.c"
        "power_event_logic.c"
        "auto_reboot.c"
        "proxy.c"
        "portmap.c"
        "${MICROLINK_DIR}/src/microlink.c"
        "${MICROLINK_DIR}/src/ml_coord.c"
        "${MICROLINK_DIR}/src/ml_derp.c"
        "${MICROLINK_DIR}/src/ml_net_io.c"
        "${MICROLINK_DIR}/src/ml_wg_mgr.c"
        "${MICROLINK_DIR}/src/ml_stun.c"
        "${MICROLINK_DIR}/src/ml_noise.c"
        "${MICROLINK_DIR}/src/ml_h2.c"
        "${MICROLINK_DIR}/src/ml_udp.c"
        "${MICROLINK_DIR}/src/ml_tcp.c"
        "${MICROLINK_DIR}/src/ml_peer_nvs.c"
        "${MICROLINK_DIR}/src/ml_cert.c"
        "${MICROLINK_DIR}/src/ml_peerapi.c"
        "${MICROLINK_DIR}/src/ml_acme.c"
        "${MICROLINK_DIR}/src/nacl_box.c"
        "${MICROLINK_DIR}/src/x25519.c"
        "${MICROLINK_DIR}/src/ml_zerocopy.c"
    INCLUDE_DIRS
        "."
        "${MICROLINK_DIR}/include"
        "${MICROLINK_DIR}/src"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_http_server
        esp_https_server
        esp_psram
        esp-tls
        lwip
        esp_timer
        json
        mbedtls
        wireguard_lwip

    PRIV_REQUIRES
        mbedtls
)

# Cellular / net_switch / config_httpd sources are excluded — we run
# microlink in WiFi-only mode (CONFIG_ML_ENABLE_CELLULAR=n by default).
# If cellular support is ever needed, drop those sources back in and add
# esp_driver_uart to REQUIRES (for ml_at_socket.c).

# Suppress noisy warnings from inlined microlink sources (treated as
# external/third-party code; we don't own these).
set_source_files_properties(
    "${MICROLINK_DIR}/src/microlink.c"
    "${MICROLINK_DIR}/src/ml_coord.c"
    "${MICROLINK_DIR}/src/ml_derp.c"
    "${MICROLINK_DIR}/src/ml_net_io.c"
    "${MICROLINK_DIR}/src/ml_wg_mgr.c"
    "${MICROLINK_DIR}/src/ml_stun.c"
    "${MICROLINK_DIR}/src/ml_noise.c"
    "${MICROLINK_DIR}/src/ml_h2.c"
    "${MICROLINK_DIR}/src/ml_udp.c"
    "${MICROLINK_DIR}/src/ml_tcp.c"
    "${MICROLINK_DIR}/src/ml_peer_nvs.c"
    "${MICROLINK_DIR}/src/ml_peerapi.c"
    "${MICROLINK_DIR}/src/nacl_box.c"
    "${MICROLINK_DIR}/src/x25519.c"
    "${MICROLINK_DIR}/src/ml_zerocopy.c"
    PROPERTIES COMPILE_FLAGS
        "-Wno-stringop-overflow -Wno-format-truncation -Wno-unused-but-set-variable"
)