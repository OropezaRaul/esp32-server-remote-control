#include "wol.h"

#include <esp_log.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/prot/ethernet.h>
#include <lwip/etharp.h>
#include <lwip/tcpip.h>
#include <string.h>
#include <stdlib.h>

#include "sdkconfig.h"

static const char *TAG = "wol";

static uint8_t cocopa01_mac[6];

static uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0;
}

esp_err_t wol_parse_mac(const char *mac_str, uint8_t out_mac[6]) {
    if (mac_str == NULL || strlen(mac_str) < 17) return ESP_ERR_INVALID_ARG;
    int idx = 0;
    for (int i = 0; i < 6; i++) {
        out_mac[i] = (uint8_t)((hex_nibble(mac_str[idx]) << 4) | hex_nibble(mac_str[idx + 1]));
        idx += 3;
        if (i < 5 && mac_str[idx - 1] != ':' && mac_str[idx - 1] != '-') {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t wol_send(const uint8_t mac[6]) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return ESP_FAIL;
    }

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    uint8_t magic[102];
    memset(magic, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(magic + 6 + i * 6, mac, 6);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;

    // 1. Ráfaga sobre broadcast de subred configurado (ej: 192.168.100.255:9)
    addr.sin_port = htons(CONFIG_COCOPA01_WOL_PORT);
    addr.sin_addr.s_addr = inet_addr(CONFIG_COCOPA01_BROADCAST_IP);
    for (int b = 0; b < 3; b++) {
        sendto(sock, magic, sizeof(magic), 0, (struct sockaddr *)&addr, sizeof(addr));
    }

    // 2. Broadcast sobre puerto 7 (Echo port - soportado por muchas tarjetas de red)
    addr.sin_port = htons(7);
    sendto(sock, magic, sizeof(magic), 0, (struct sockaddr *)&addr, sizeof(addr));

    // 3. Broadcast universal 255.255.255.255 en puertos 9 y 7
    addr.sin_port = htons(CONFIG_COCOPA01_WOL_PORT);
    addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    sendto(sock, magic, sizeof(magic), 0, (struct sockaddr *)&addr, sizeof(addr));
    addr.sin_port = htons(7);
    sendto(sock, magic, sizeof(magic), 0, (struct sockaddr *)&addr, sizeof(addr));

    // 4. Unicast directo a la IP del servidor (bypasea cualquier aislamiento WiFi del router)
    if (strlen(CONFIG_COCOPA01_IP) > 0) {
#if ETHARP_SUPPORT_STATIC_ENTRIES
        ip4_addr_t target_ip;
        if (ip4addr_aton(CONFIG_COCOPA01_IP, &target_ip)) {
            struct eth_addr eth;
            memcpy(eth.addr, mac, 6);
            LOCK_TCPIP_CORE();
            err_t aerr = etharp_add_static_entry(&target_ip, &eth);
            UNLOCK_TCPIP_CORE();
            if (aerr == ERR_OK) {
                ESP_LOGD(TAG, "static ARP entry ensured: %s -> %02X:%02X:%02X:%02X:%02X:%02X",
                         CONFIG_COCOPA01_IP, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            } else {
                ESP_LOGW(TAG, "etharp_add_static_entry failed: %d", (int)aerr);
            }
        }
#endif
        addr.sin_port = htons(CONFIG_COCOPA01_WOL_PORT);
        addr.sin_addr.s_addr = inet_addr(CONFIG_COCOPA01_IP);
        for (int b = 0; b < 3; b++) {
            sendto(sock, magic, sizeof(magic), 0, (struct sockaddr *)&addr, sizeof(addr));
        }
    }

    close(sock);

    ESP_LOGI(TAG, "WOL multi-destination burst sent: %02X:%02X:%02X:%02X:%02X:%02X (ports 9,7 -> %s, 255.255.255.255, %s)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             CONFIG_COCOPA01_BROADCAST_IP, CONFIG_COCOPA01_IP);
    return ESP_OK;
}

esp_err_t wol_send_str(const char *mac_str) {
    uint8_t mac[6];
    esp_err_t err = wol_parse_mac(mac_str, mac);
    if (err != ESP_OK) return err;
    return wol_send(mac);
}

esp_err_t wol_send_default(void) {
    static bool loaded = false;
    if (!loaded) {
        esp_err_t err = wol_parse_mac(CONFIG_COCOPA01_MAC_HEX, cocopa01_mac);
        if (err != ESP_OK) return err;
        loaded = true;
    }
    return wol_send(cocopa01_mac);
}