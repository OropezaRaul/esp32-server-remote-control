






















#include "microlink_internal.h"
#include "x25519.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "ml_coord";


#define CTRL_HOST(ml) ((ml)->ctrl_host[0] ? (ml)->ctrl_host : ML_CTRL_HOST)


static uint8_t s_node_key_challenge[32] = {0};
static bool s_has_node_key_challenge = false;


typedef enum {
    COORD_IDLE,
    COORD_STUN_PROBE,
    COORD_DNS_RESOLVE,
    COORD_TCP_CONNECT,
    COORD_NOISE_HANDSHAKE,
    COORD_H2_PREFACE,
    COORD_REGISTER,
    COORD_FETCH_PEERS,
    COORD_LONG_POLL,
    COORD_RECONNECTING,
} coord_state_t;





static void bytes_to_hex(const uint8_t *bytes, size_t len, char *hex) {
    static const char hextab[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        hex[i * 2] = hextab[bytes[i] >> 4];
        hex[i * 2 + 1] = hextab[bytes[i] & 0x0F];
    }
    hex[len * 2] = '\0';
}

static int hex_to_bytes(const char *hex, uint8_t *bytes, size_t max_len) {
    size_t hex_len = strlen(hex);
    if (hex_len % 2 != 0 || hex_len / 2 > max_len) return -1;
    for (size_t i = 0; i < hex_len / 2; i++) {
        unsigned int val;
        if (sscanf(hex + i * 2, "%2x", &val) != 1) return -1;
        bytes[i] = (uint8_t)val;
    }
    return hex_len / 2;
}





static int coord_send(microlink_t *ml, const uint8_t *data, size_t len) {
    
    struct timeval snd_tv = { .tv_sec = 5, .tv_usec = 0 };
    ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));

    size_t sent = 0;
    while (sent < len) {
        int n = ml_send(ml->coord_sock, data + sent, len - sent, 0);
        if (n <= 0) {
            ESP_LOGE(TAG, "coord_send failed: sent=%d/%d n=%d errno=%d",
                     (int)sent, (int)len, n, errno);
            return -1;
        }
        sent += n;
    }
    return 0;
}

static int coord_recv(microlink_t *ml, uint8_t *buf, size_t len) {
    size_t recvd = 0;
    int retries = 0;
    while (recvd < len) {
        int n = ml_recv(ml->coord_sock, buf + recvd, len - recvd, 0);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (recvd == 0) {
                    
                    return -1;
                }
                

                if (++retries > 300) {  
                    ESP_LOGE(TAG, "coord_recv partial timeout: %d/%d bytes",
                             (int)recvd, (int)len);
                    return -1;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            ESP_LOGE(TAG, "coord_recv failed: %d (errno %d, recvd %d/%d)",
                     n, errno, (int)recvd, (int)len);
            return -1;
        }
        recvd += n;
        retries = 0;  
        




        ml->last_ctrl_rx_ms = (uint32_t)ml_get_time_ms();
    }
    return 0;
}






static int noise_send(microlink_t *ml, ml_noise_state_t *noise,
                        const uint8_t *plaintext, size_t pt_len) {
    size_t ct_len = pt_len + 16;  
    uint8_t *frame = ml_psram_malloc(3 + ct_len);
    if (!frame) return -1;

    frame[0] = 0x04;  
    frame[1] = (ct_len >> 8) & 0xFF;
    frame[2] = ct_len & 0xFF;

    if (ml_noise_encrypt(noise->tx_key, noise->tx_nonce,
                          NULL, 0,
                          plaintext, pt_len,
                          frame + 3) != ESP_OK) {
        free(frame);
        return -1;
    }
    noise->tx_nonce++;

    int ret = coord_send(ml, frame, 3 + ct_len);
    free(frame);
    return ret;
}


static int noise_recv(microlink_t *ml, ml_noise_state_t *noise,
                        uint8_t *plaintext, size_t max_len) {
    
    uint8_t hdr[3];
    if (coord_recv(ml, hdr, 3) < 0) return -1;

    if (hdr[0] != 0x04) {
        ESP_LOGE(TAG, "Unexpected Noise frame type: 0x%02x", hdr[0]);
        return -1;
    }

    uint16_t ct_len = (hdr[1] << 8) | hdr[2];
    if (ct_len < 16) return -1;
    size_t pt_len = ct_len - 16;
    if (pt_len > max_len) {
        ESP_LOGE(TAG, "Noise frame too large: %d > %d", (int)pt_len, (int)max_len);
        return -1;
    }

    uint8_t *ciphertext = ml_psram_malloc(ct_len);
    if (!ciphertext) return -1;

    


    int payload_retries = 0;
    while (coord_recv(ml, ciphertext, ct_len) < 0) {
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && ++payload_retries <= 300) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        ESP_LOGE(TAG, "noise_recv payload failed: ct_len=%d retries=%d errno=%d",
                 ct_len, payload_retries, errno);
        free(ciphertext);
        return -1;
    }

    if (ml_noise_decrypt(noise->rx_key, noise->rx_nonce,
                          NULL, 0,
                          ciphertext, ct_len,
                          plaintext) != ESP_OK) {
        ESP_LOGE(TAG, "Noise decrypt failed (nonce=%llu)", (unsigned long long)noise->rx_nonce);
        free(ciphertext);
        return -1;
    }
    noise->rx_nonce++;

    free(ciphertext);
    return (int)pt_len;
}





static int do_tcp_connect(microlink_t *ml) {
    int64_t t_start = esp_timer_get_time();

    ESP_LOGI(TAG, "Resolving %s...", CTRL_HOST(ml));

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;

    if (ml_getaddrinfo(CTRL_HOST(ml), "80", &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolve failed for %s", CTRL_HOST(ml));
        return -1;
    }

    int64_t t_dns = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DNS resolve: %lld ms", (t_dns - t_start) / 1000);

    int sock = ml_socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ml_freeaddrinfo(res);
        ESP_LOGE(TAG, "Socket creation failed");
        return -1;
    }

    
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    
    int keepalive = 1;
    int keepidle = 25;
    int keepintvl = 10;
    int keepcnt = 3;
    ml_setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    ml_setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    ml_setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    ml_setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    ESP_LOGI(TAG, "Connecting to %s:80...", CTRL_HOST(ml));

    if (ml_connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        ESP_LOGE(TAG, "TCP connect failed: %d", errno);
        ml_close_sock(sock);
        ml_freeaddrinfo(res);
        return -1;
    }
    ml_freeaddrinfo(res);

    int64_t t_tcp = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] TCP connect: %lld ms (total: %lld ms)",
             (t_tcp - t_dns) / 1000, (t_tcp - t_start) / 1000);

    ml->coord_sock = sock;
    return 0;
}






static uint8_t *s_server_extra_data = NULL;
static int s_server_extra_data_len = 0;

static int do_noise_handshake(microlink_t *ml, ml_noise_state_t *noise) {
    int64_t t_noise_start = esp_timer_get_time();

    
    ml_noise_init(noise,
                   ml->machine_private_key, ml->machine_public_key,
                   NULL);  

    
    uint8_t msg1[128];
    size_t msg1_len = 0;
    if (ml_noise_write_msg1(noise, msg1, &msg1_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build Noise msg1");
        return -1;
    }

    
    char msg1_b64[256];
    size_t b64_len = 0;
    mbedtls_base64_encode((unsigned char *)msg1_b64, sizeof(msg1_b64), &b64_len,
                           msg1, msg1_len);
    msg1_b64[b64_len] = '\0';

    
    char *http_req = ml_psram_malloc(512 + b64_len);
    if (!http_req) return -1;

    int req_len = snprintf(http_req, 512 + b64_len,
        "POST /ts2021 HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: tailscale-control-protocol\r\n"
        "Connection: Upgrade\r\n"
        "User-Agent: Tailscale\r\n"
        "X-Tailscale-Handshake: %s\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        CTRL_HOST(ml), msg1_b64);

    ESP_LOGI(TAG, "Sending Noise handshake (msg1=%d bytes, b64=%d chars)", (int)msg1_len, (int)b64_len);

    if (coord_send(ml, (uint8_t *)http_req, req_len) < 0) {
        free(http_req);
        return -1;
    }
    free(http_req);

    

    uint8_t *resp = ml_psram_malloc(2048);
    if (!resp) return -1;

    int total = ml_recv(ml->coord_sock, resp, 2047, 0);
    if (total <= 0) {
        ESP_LOGE(TAG, "Handshake recv failed: %d (errno=%d)", total, errno);
        free(resp);
        return -1;
    }
    resp[total] = '\0';

    ESP_LOGI(TAG, "HTTP response received: %d bytes", total);

    
    if (strstr((char *)resp, "101") == NULL) {
        ESP_LOGE(TAG, "Noise upgrade rejected: %.200s", resp);
        free(resp);
        return -1;
    }
    ESP_LOGI(TAG, "HTTP 101 received, looking for Noise msg2...");

    
    uint8_t *body_start = (uint8_t *)strstr((char *)resp, "\r\n\r\n");
    if (!body_start) {
        ESP_LOGE(TAG, "No header terminator found");
        free(resp);
        return -1;
    }
    body_start += 4; 

    int body_len = total - (body_start - resp);
    ESP_LOGI(TAG, "Body after HTTP headers: %d bytes", body_len);

    
    uint8_t *body_buf = NULL;
    int body_buf_len = 0;

    if (body_len > 0) {
        body_buf = ml_psram_malloc(body_len);
        if (body_buf) {
            memcpy(body_buf, body_start, body_len);
            body_buf_len = body_len;
        }
    }
    free(resp);

    
    if (body_buf_len < 3) {
        ESP_LOGI(TAG, "Reading Noise msg2 header from socket (have %d bytes)...", body_buf_len);
        
        if (!body_buf) {
            body_buf = ml_psram_malloc(512);
            if (!body_buf) return -1;
        }
        if (coord_recv(ml, body_buf + body_buf_len, 3 - body_buf_len) < 0) {
            ESP_LOGE(TAG, "Failed to read Noise msg2 header");
            free(body_buf);
            return -1;
        }
        body_buf_len = 3;
    }

    
    uint8_t msg_type = body_buf[0];
    uint16_t payload_len = (body_buf[1] << 8) | body_buf[2];

    ESP_LOGI(TAG, "Noise frame: type=0x%02x, payload_len=%d", msg_type, payload_len);

    if (msg_type != 0x02) {
        ESP_LOGE(TAG, "Unexpected Noise msg type: 0x%02x (expected 0x02)", msg_type);
        free(body_buf);
        return -1;
    }

    int msg2_total = 3 + payload_len;  

    
    if (body_buf_len < msg2_total) {
        ESP_LOGI(TAG, "Reading remaining msg2 payload: have %d, need %d", body_buf_len, msg2_total);
        
        uint8_t *tmp = ml_psram_malloc(msg2_total + 512);
        if (!tmp) { free(body_buf); return -1; }
        memcpy(tmp, body_buf, body_buf_len);
        free(body_buf);
        body_buf = tmp;

        if (coord_recv(ml, body_buf + body_buf_len, msg2_total - body_buf_len) < 0) {
            ESP_LOGE(TAG, "Failed to read Noise msg2 payload");
            free(body_buf);
            return -1;
        }
        body_buf_len = msg2_total;
    }

    ESP_LOGI(TAG, "Noise msg2 complete: %d bytes (payload=%d)", msg2_total, payload_len);

    
    if (ml_noise_read_msg2(noise, body_buf + 3, payload_len) != ESP_OK) {
        ESP_LOGE(TAG, "Noise msg2 processing failed");
        free(body_buf);
        return -1;
    }

    int64_t t_noise_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] Noise handshake: %lld ms", (t_noise_done - t_noise_start) / 1000);
    ESP_LOGI(TAG, "Noise handshake complete, transport keys derived");

    








    int extra_len = body_buf_len - msg2_total;
    uint8_t *extra_data = NULL;

    if (extra_len > 0) {
        
        ESP_LOGI(TAG, "Found %d bytes of extra data after msg2 (proactive frames)", extra_len);
        extra_data = ml_psram_malloc(extra_len);
        if (extra_data) {
            memcpy(extra_data, body_buf + msg2_total, extra_len);
        }
    } else {
        



        ESP_LOGI(TAG, "No extra data in initial buffer, reading proactive frames from socket...");
        struct timeval short_tv = { .tv_sec = 2, .tv_usec = 0 };
        ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_RCVTIMEO, &short_tv, sizeof(short_tv));

        extra_data = ml_psram_malloc(1024);
        if (extra_data) {
            int n = ml_recv(ml->coord_sock, extra_data, 1024, 0);
            if (n > 0) {
                extra_len = n;
                ESP_LOGI(TAG, "Read %d bytes of proactive frames from socket", extra_len);
            } else {
                ESP_LOGI(TAG, "No proactive frames from server (n=%d errno=%d)", n, errno);
                free(extra_data);
                extra_data = NULL;
                extra_len = 0;
            }
        }

        
        struct timeval normal_tv = { .tv_sec = 60, .tv_usec = 0 };
        ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_RCVTIMEO, &normal_tv, sizeof(normal_tv));
    }

    if (extra_data && extra_len > 0) {
        
        int offset = 0;
        int frame_count = 0;
        while (offset + 3 <= extra_len) {
            uint8_t ft = extra_data[offset];
            uint16_t fl = (extra_data[offset + 1] << 8) | extra_data[offset + 2];
            if (offset + 3 + fl > extra_len) {
                ESP_LOGW(TAG, "Incomplete proactive frame at offset %d", offset);
                break;
            }
            ESP_LOGI(TAG, "Proactive frame %d: type=0x%02x, len=%u", frame_count, ft, fl);
            frame_count++;
            offset += 3 + fl;
        }
        ESP_LOGI(TAG, "Server sent %d proactive transport frames", frame_count);

        
        if (s_server_extra_data) free(s_server_extra_data);
        s_server_extra_data = extra_data;
        s_server_extra_data_len = extra_len;
    } else {
        if (extra_data) free(extra_data);
    }

    free(body_buf);
    return 0;
}








static void process_proactive_frames(microlink_t *ml, ml_noise_state_t *noise) {
    if (!s_server_extra_data || s_server_extra_data_len <= 0) {
        return;
    }

    ESP_LOGI(TAG, "Processing %d bytes of server proactive data", s_server_extra_data_len);
    s_has_node_key_challenge = false;

    






    uint8_t *combined = ml_psram_malloc(1024);
    size_t combined_len = 0;
    int offset = 0;
    int frame_count = 0;
    uint64_t nonce = 0;

    while (offset + 3 <= s_server_extra_data_len) {
        uint8_t ft = s_server_extra_data[offset];
        uint16_t fl = (s_server_extra_data[offset + 1] << 8) | s_server_extra_data[offset + 2];

        if (offset + 3 + fl > s_server_extra_data_len) break;

        ESP_LOGI(TAG, "Proactive frame %d: type=0x%02x, len=%u", frame_count, ft, fl);

        if (ft == 0x04 && fl >= ML_NOISE_MAC_LEN && combined) {
            size_t pt_len = fl - ML_NOISE_MAC_LEN;
            uint8_t *plaintext = ml_psram_malloc(pt_len + 1);

            if (plaintext) {
                esp_err_t ret = ml_noise_decrypt(noise->rx_key, nonce,
                                                  NULL, 0,
                                                  s_server_extra_data + offset + 3, fl,
                                                  plaintext);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "  Decrypted frame %d: %d bytes (nonce=%d)",
                             frame_count, (int)pt_len, (int)nonce);
                    if (combined_len + pt_len < 1024) {
                        memcpy(combined + combined_len, plaintext, pt_len);
                        combined_len += pt_len;
                    }
                } else {
                    ESP_LOGW(TAG, "  Frame %d decrypt failed (nonce=%d)", frame_count, (int)nonce);
                }
                free(plaintext);
            }
        }

        nonce++;
        frame_count++;
        offset += 3 + fl;
    }

    ESP_LOGI(TAG, "Decrypted %d bytes from %d proactive frames", (int)combined_len, frame_count);

    
    if (combined && combined_len > 0) {
        combined[combined_len < 1024 ? combined_len : 1023] = '\0';

        
        char *json_start = NULL;
        for (int i = 0; i < (int)combined_len; i++) {
            if (combined[i] == '{') {
                json_start = (char *)combined + i;
                break;
            }
        }

        if (json_start) {
            ESP_LOGI(TAG, "EarlyNoise JSON: %.80s", json_start);
            cJSON *early_json = cJSON_Parse(json_start);
            if (early_json) {
                cJSON *challenge = cJSON_GetObjectItem(early_json, "nodeKeyChallenge");
                if (challenge && cJSON_IsString(challenge)) {
                    const char *chal_str = challenge->valuestring;
                    ESP_LOGI(TAG, "Found nodeKeyChallenge: %.40s...", chal_str);

                    
                    if (strncmp(chal_str, "chalpub:", 8) == 0) {
                        const char *hex = chal_str + 8;
                        if (strlen(hex) >= 64) {
                            hex_to_bytes(hex, s_node_key_challenge, 32);
                            s_has_node_key_challenge = true;
                            ESP_LOGI(TAG, "NodeKeyChallenge decoded (32 bytes)");
                        }
                    }
                }
                cJSON_Delete(early_json);
            }
        }
    }
    if (combined) free(combined);

    
    ESP_LOGI(TAG, "Setting rx_nonce=%d (skipping %d proactive frames)", frame_count, frame_count);
    noise->rx_nonce = frame_count;

    free(s_server_extra_data);
    s_server_extra_data = NULL;
    s_server_extra_data_len = 0;
}





static int do_h2_preface(microlink_t *ml, ml_noise_state_t *noise) {
    int64_t t_h2_start = esp_timer_get_time();

    

    uint8_t h2_init[128];
    int pos = 0;

    int preface_len = ml_h2_build_preface(h2_init, sizeof(h2_init));
    if (preface_len < 0) return -1;
    pos = preface_len;

    int ack_len = ml_h2_build_settings_ack(h2_init + pos, sizeof(h2_init) - pos);
    if (ack_len < 0) return -1;
    pos += ack_len;

    



    uint32_t conn_window_delta = ML_H2_BUFFER_SIZE - 65535;
    if (conn_window_delta > 0) {
        int wu_len = ml_h2_build_window_update(h2_init + pos, sizeof(h2_init) - pos,
                                                0, conn_window_delta);
        if (wu_len > 0) pos += wu_len;
    }

    
    if (noise_send(ml, noise, h2_init, pos) < 0) {
        ESP_LOGE(TAG, "Failed to send H2 preface");
        return -1;
    }

    ESP_LOGI(TAG, "H2 preface sent (%d bytes, conn window=%luKB)",
             pos, (unsigned long)(ML_H2_BUFFER_SIZE / 1024));

    
    uint8_t recv_buf[4096];
    int recv_len = noise_recv(ml, noise, recv_buf, sizeof(recv_buf));
    if (recv_len < 0) {
        ESP_LOGW(TAG, "No immediate H2 response from server (may come later)");
        
    } else {
        ESP_LOGI(TAG, "Server H2 response: %d bytes", recv_len);
    }

    int64_t t_h2_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] H2 preface: %lld ms", (t_h2_done - t_h2_start) / 1000);

    return 0;
}















static void add_funnel_hostinfo(microlink_t *ml, cJSON *hostinfo) {
    if (!ml->config.enable_funnel) return;

    cJSON_AddBoolToObject(hostinfo, "IngressEnabled", true);
    cJSON_AddBoolToObject(hostinfo, "WireIngress", true);

    if (ml->config.peerapi_port != 0) {
        cJSON *services = cJSON_AddArrayToObject(hostinfo, "Services");
        if (services) {
            cJSON *svc4 = cJSON_CreateObject();
            if (svc4) {
                cJSON_AddStringToObject(svc4, "Proto", "peerapi4");
                cJSON_AddNumberToObject(svc4, "Port", ml->config.peerapi_port);
                cJSON_AddItemToArray(services, svc4);
            }
#if LWIP_IPV6
            cJSON *svc6 = cJSON_CreateObject();
            if (svc6) {
                cJSON_AddStringToObject(svc6, "Proto", "peerapi6");
                cJSON_AddNumberToObject(svc6, "Port", ml->config.peerapi_port);
                cJSON_AddItemToArray(services, svc6);
            }
#endif
        }
        ESP_LOGI(TAG, "Hostinfo: IngressEnabled=true, peerapi4+6 port=%u (Funnel advertised)",
                 (unsigned)ml->config.peerapi_port);
    } else {
        ESP_LOGI(TAG, "Hostinfo: IngressEnabled=true (Funnel advertised)");
    }
}

static void parse_self_addresses(microlink_t *ml, cJSON *addresses) {
    if (!addresses || !cJSON_IsArray(addresses)) return;
    cJSON *item;
    cJSON_ArrayForEach(item, addresses) {
        if (!item->valuestring) continue;
        const char *addr_str = item->valuestring;
        unsigned a, b, c, d;
        if (sscanf(addr_str, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            uint32_t ip = (a << 24) | (b << 16) | (c << 8) | d;
            if (ml->vpn_ip != ip) {
                ml->vpn_ip = ip;
                char ip_buf[16];
                microlink_ip_to_str(ml->vpn_ip, ip_buf);
                ESP_LOGI(TAG, "Our VPN IPv4: %s", ip_buf);
            }
            continue;
        }
#if LWIP_IPV6
        char clean_v6[64];
        strlcpy(clean_v6, addr_str, sizeof(clean_v6));
        char *slash = strchr(clean_v6, '/');
        if (slash) *slash = '\0';
        ip6_addr_t v6;
        if (ip6addr_aton(clean_v6, &v6)) {
            memcpy((void *)&ml->vpn_ipv6, &v6, sizeof(ip6_addr_t));
            char v6_str[48];
            ip6addr_ntoa_r(&ml->vpn_ipv6, v6_str, sizeof(v6_str));
            ESP_LOGI(TAG, "Our VPN IPv6: %s", v6_str);
        }
#endif
    }
}

static void add_netinfo_to_hostinfo(microlink_t *ml, cJSON *hostinfo) {
    if (!hostinfo) return;
    cJSON *netinfo = cJSON_CreateObject();
    if (!netinfo) return;

    cJSON_AddNumberToObject(netinfo, "PreferredDERP", ML_DERP_REGION);
    cJSON_AddBoolToObject(netinfo, "WorkingUDP", true);
    cJSON_AddBoolToObject(netinfo, "WorkingIPv6", true);
    cJSON_AddBoolToObject(netinfo, "OSHasIPv6", true);

    if (ml->stun_nat_checked) {
        cJSON_AddBoolToObject(netinfo, "MappingVariesByDestIP", ml->nat_mapping_varies);
    }

    cJSON *derp_lat = cJSON_CreateObject();
    if (derp_lat) {
        cJSON_AddNumberToObject(derp_lat, "9-v4", 0.080);
        cJSON_AddItemToObject(netinfo, "DERPLatency", derp_lat);
    }

    cJSON_AddItemToObject(hostinfo, "NetInfo", netinfo);
}

static int do_register(microlink_t *ml, ml_noise_state_t *noise) {
    int64_t t_reg_start = esp_timer_get_time();

    
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddNumberToObject(root, "Version", ML_CTRL_PROTOCOL_VER);

    
    char key_hex[65];
    char key_str[80];
    bytes_to_hex(ml->wg_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "nodekey:%s", key_hex);
    cJSON_AddStringToObject(root, "NodeKey", key_str);

    
    if (ml->config.auth_key && strlen(ml->config.auth_key) > 0) {
        cJSON *auth = cJSON_CreateObject();
        cJSON_AddStringToObject(auth, "AuthKey", ml->config.auth_key);
        cJSON_AddItemToObject(root, "Auth", auth);
    }

    
    cJSON *hostinfo = cJSON_CreateObject();
    const char *dev_name = (ml->config.device_name && ml->config.device_name[0]) ? ml->config.device_name : microlink_default_device_name();
    cJSON_AddStringToObject(hostinfo, "Hostname", dev_name);
    cJSON_AddStringToObject(hostinfo, "OS", "linux");
    cJSON_AddStringToObject(hostinfo, "OSVersion", "ESP-IDF");
    cJSON_AddStringToObject(hostinfo, "GoArch", "arm");

    





    cJSON_AddStringToObject(hostinfo, "IPNVersion", MICROLINK_IPN_VERSION);
    cJSON_AddStringToObject(hostinfo, "App", MICROLINK_APP_NAME);

    

    add_funnel_hostinfo(ml, hostinfo);

    
    add_netinfo_to_hostinfo(ml, hostinfo);

    cJSON_AddItemToObject(root, "Hostinfo", hostinfo);

    


    if (s_has_node_key_challenge) {
        ESP_LOGI(TAG, "Computing NodeKeyChallengeResponse...");

        
        uint8_t challenge_pub_copy[32];
        memcpy(challenge_pub_copy, s_node_key_challenge, 32);
        challenge_pub_copy[31] &= 0x7F;  

        uint8_t challenge_response[32];
        x25519(challenge_response, ml->wg_private_key, challenge_pub_copy, 1);

        
        char chalresp_hex[65];
        bytes_to_hex(challenge_response, 32, chalresp_hex);
        char chalresp_str[80];
        snprintf(chalresp_str, sizeof(chalresp_str), "chalresp:%s", chalresp_hex);
        cJSON_AddStringToObject(root, "NodeKeyChallengeResponse", chalresp_str);
        ESP_LOGI(TAG, "Added NodeKeyChallengeResponse");
    } else {
        ESP_LOGW(TAG, "No nodeKeyChallenge received - registration may fail!");
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return -1;

    size_t json_len = strlen(json_str);
    ESP_LOGI(TAG, "RegisterRequest: %d bytes", (int)json_len);

    
    uint8_t *h2_buf = ml_psram_malloc(json_len + 512);
    if (!h2_buf) { free(json_str); return -1; }

    int h2_pos = 0;

    
    int hdr_len = ml_h2_build_headers_frame(h2_buf + h2_pos, json_len + 512 - h2_pos,
                                              "POST", "/machine/register",
                                              CTRL_HOST(ml), "application/json",
                                              1, false);
    if (hdr_len < 0) { free(json_str); free(h2_buf); return -1; }
    h2_pos += hdr_len;

    
    int data_len = ml_h2_build_data_frame(h2_buf + h2_pos, json_len + 512 - h2_pos,
                                            (uint8_t *)json_str, json_len,
                                            1, true);
    free(json_str);
    if (data_len < 0) { free(h2_buf); return -1; }
    h2_pos += data_len;

    
    if (noise_send(ml, noise, h2_buf, h2_pos) < 0) {
        free(h2_buf);
        return -1;
    }
    free(h2_buf);

    int64_t t_reg_sent = esp_timer_get_time();
    ESP_LOGI(TAG, "RegisterRequest sent (%d H2 bytes) [TIMING] send: %lld ms",
             h2_pos, (t_reg_sent - t_reg_start) / 1000);

    

    uint8_t *h2_resp = ml_psram_malloc(16384);
    if (!h2_resp) return -1;
    size_t h2_resp_len = 0;

    uint8_t *resp_buf = ml_psram_malloc(8192);
    if (!resp_buf) { free(h2_resp); return -1; }
    size_t resp_total = 0;

    


    bool got_register_end = false;
    for (int frame_count = 0; frame_count < 10 && !got_register_end; frame_count++) {
        uint8_t *frame_buf = ml_psram_malloc(4096);
        if (!frame_buf) break;

        int frame_len = noise_recv(ml, noise, frame_buf, 4096);
        if (frame_len <= 0) {
            free(frame_buf);
            break;
        }

        ESP_LOGI(TAG, "RegisterResponse Noise frame %d: %d bytes", frame_count, frame_len);

        if (h2_resp_len + frame_len < 16384) {
            memcpy(h2_resp + h2_resp_len, frame_buf, frame_len);
            h2_resp_len += frame_len;
        }
        free(frame_buf);

        
        int scan = 0;
        while (scan + 9 <= (int)h2_resp_len) {
            uint32_t fl = (h2_resp[scan] << 16) | (h2_resp[scan+1] << 8) | h2_resp[scan+2];
            uint8_t ft = h2_resp[scan+3];
            uint8_t ff = h2_resp[scan+4];
            uint32_t fs = ((h2_resp[scan+5] & 0x7F) << 24) | (h2_resp[scan+6] << 16) |
                          (h2_resp[scan+7] << 8) | h2_resp[scan+8];
            if (fl > 1000000 || scan + 9 + (int)fl > (int)h2_resp_len) break;
            
            if (fs == 1 && (ft == 0x00 || ft == 0x01) && (ff & 0x01)) {
                got_register_end = true;
                break;
            }
            scan += 9 + fl;
        }
    }

    
    bool got_end_stream = false;
    int fpos = 0;
    while (fpos + 9 <= (int)h2_resp_len) {
        uint32_t f_len = (h2_resp[fpos] << 16) | (h2_resp[fpos + 1] << 8) | h2_resp[fpos + 2];
        uint8_t f_type = h2_resp[fpos + 3];
        uint8_t f_flags = h2_resp[fpos + 4];
        uint32_t f_stream = ((h2_resp[fpos + 5] & 0x7F) << 24) | (h2_resp[fpos + 6] << 16) |
                             (h2_resp[fpos + 7] << 8) | h2_resp[fpos + 8];
        fpos += 9;

        ESP_LOGD(TAG, "  Register H2 frame: type=%d flags=0x%02x len=%lu stream=%lu",
                 f_type, f_flags, (unsigned long)f_len, (unsigned long)f_stream);

        if (f_len > 1000000 || fpos + (int)f_len > (int)h2_resp_len) {
            ESP_LOGW(TAG, "  Invalid H2 frame length %lu at pos %d, stopping", (unsigned long)f_len, fpos - 9);
            break;
        }

        

        if (f_stream == 1) {
            if (f_type == 0x00 && f_len > 0) {  
                if (resp_total + f_len < 8192) {
                    memcpy(resp_buf + resp_total, h2_resp + fpos, f_len);
                    resp_total += f_len;
                }
                if (f_flags & 0x01) got_end_stream = true;
            }
            if (f_type == 0x01 && (f_flags & 0x01)) got_end_stream = true;
        }

        fpos += f_len;
    }
    free(h2_resp);

    

    if (resp_total > 0) {
        uint8_t wu_buf[13];
        int wu_len = ml_h2_build_window_update(wu_buf, 13, 0, (uint32_t)resp_total);
        noise_send(ml, noise, wu_buf, wu_len);
    }

    ESP_LOGI(TAG, "RegisterResponse: %d bytes total data", (int)resp_total);

    if (resp_total == 0) {
        ESP_LOGW(TAG, "No DATA frame in RegisterResponse");
        free(resp_buf);
        
        return 0;
    }

    uint8_t *json_data = resp_buf;
    size_t json_data_len = resp_total;

    
    {
        int dump = json_data_len < 32 ? (int)json_data_len : 32;
        char hexbuf[97];
        for (int i = 0; i < dump; i++) {
            sprintf(hexbuf + i * 3, "%02x ", json_data[i]);
        }
        hexbuf[dump * 3] = '\0';
        ESP_LOGI(TAG, "RegisterResponse first %d bytes (hex): %s", dump, hexbuf);
    }

    
    char *parse_start = (char *)json_data;
    size_t parse_len = json_data_len;
    int json_offset = -1;
    for (int i = 0; i < 8 && i < (int)json_data_len; i++) {
        if (json_data[i] == '{') {
            json_offset = i;
            break;
        }
    }
    if (json_offset > 0) {
        ESP_LOGI(TAG, "RegisterResponse JSON starts at offset %d", json_offset);
        parse_start += json_offset;
        parse_len -= json_offset;
    } else if (json_offset < 0) {
        ESP_LOGW(TAG, "No '{' found in RegisterResponse data");
        free(resp_buf);
        return 0;
    }

    
    char saved = parse_start[parse_len];
    parse_start[parse_len] = '\0';

    cJSON *resp_json = cJSON_Parse(parse_start);
    if (!resp_json) {
        ESP_LOGW(TAG, "Failed to parse RegisterResponse JSON (len=%d)", (int)parse_len);
        ESP_LOGW(TAG, "First 100 chars: %.100s", parse_start);
        parse_start[parse_len] = saved;
        free(resp_buf);
        return 0;  
    }
    parse_start[parse_len] = saved;
    free(resp_buf);

    
    cJSON *node = cJSON_GetObjectItem(resp_json, "Node");
    if (node) {
        

        cJSON *self_name = cJSON_GetObjectItem(node, "Name");
        if (self_name && self_name->valuestring && self_name->valuestring[0]) {
            strncpy(ml->self_fqdn, self_name->valuestring, sizeof(ml->self_fqdn) - 1);
            ml->self_fqdn[sizeof(ml->self_fqdn) - 1] = '\0';
            size_t flen = strlen(ml->self_fqdn);
            if (flen > 0 && ml->self_fqdn[flen - 1] == '.') {
                ml->self_fqdn[flen - 1] = '\0';
            }
            ESP_LOGI(TAG, "Self FQDN: %s", ml->self_fqdn);
        }
        cJSON *addresses = cJSON_GetObjectItem(node, "Addresses");
        parse_self_addresses(ml, addresses);
        

        cJSON *home_derp = cJSON_GetObjectItem(node, "HomeDERP");
        if (home_derp && cJSON_IsNumber(home_derp) && home_derp->valueint > 0) {
            ml->derp_home_region = (uint16_t)home_derp->valueint;
            ESP_LOGI(TAG, "Home DERP region: %d (from server, HomeDERP)", ml->derp_home_region);
        } else {
            cJSON *self_derp = cJSON_GetObjectItem(node, "DERP");
            if (self_derp && self_derp->valuestring) {
                ESP_LOGI(TAG, "Self-Node DERP: %s", self_derp->valuestring);
                const char *colon = strrchr(self_derp->valuestring, ':');
                if (colon) {
                    int region = atoi(colon + 1);
                    if (region > 0) {
                        ml->derp_home_region = (uint16_t)region;
                        ESP_LOGI(TAG, "Home DERP region: %d (from server, legacy DERP)", region);
                    }
                }
            }
        }
        
        if (ml->derp_home_region == 0) {
            ml->derp_home_region = ML_DERP_REGION;
            ESP_LOGI(TAG, "Home DERP region: %d (default)", ML_DERP_REGION);
        }
        cJSON *self_key = cJSON_GetObjectItem(node, "Key");
        if (self_key && self_key->valuestring) {
            ESP_LOGI(TAG, "Self-Node Key (from server): %s", self_key->valuestring);
            char our_key_hex[65];
            bytes_to_hex(ml->wg_public_key, 32, our_key_hex);
            ESP_LOGI(TAG, "Our WG pubkey (local):       nodekey:%s", our_key_hex);
        }
    }

    cJSON_Delete(resp_json);

    int64_t t_reg_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] Register total: %lld ms", (t_reg_done - t_reg_start) / 1000);

    return 0;
}





static void parse_peers_from_map_response(microlink_t *ml, cJSON *root) {
    



    {
        cJSON *self_node = cJSON_GetObjectItem(root, "Node");
        if (self_node) {
            

            cJSON *self_name = cJSON_GetObjectItem(self_node, "Name");
            if (self_name && self_name->valuestring && self_name->valuestring[0] &&
                ml->self_fqdn[0] == '\0') {
                strncpy(ml->self_fqdn, self_name->valuestring, sizeof(ml->self_fqdn) - 1);
                ml->self_fqdn[sizeof(ml->self_fqdn) - 1] = '\0';
                size_t flen = strlen(ml->self_fqdn);
                if (flen > 0 && ml->self_fqdn[flen - 1] == '.') {
                    ml->self_fqdn[flen - 1] = '\0';
                }
                ESP_LOGI(TAG, "Self FQDN: %s", ml->self_fqdn);
            }
            cJSON *caps = cJSON_GetObjectItem(self_node, "Capabilities");
            cJSON *capmap = cJSON_GetObjectItem(self_node, "CapMap");
            if (caps && cJSON_IsArray(caps)) {
                int n = cJSON_GetArraySize(caps);
                for (int i = 0; i < n; i++) {
                    cJSON *c = cJSON_GetArrayItem(caps, i);
                    if (c && cJSON_IsString(c)) {
                        ESP_LOGI(TAG, "nodecap[%d]: %s", i, c->valuestring ? c->valuestring : "?");
                    }
                }
            }
            if (capmap && cJSON_IsObject(capmap)) {
                cJSON *c = NULL;
                cJSON_ArrayForEach(c, capmap) {
                    ESP_LOGI(TAG, "nodecap: %s", c->string ? c->string : "?");
                }
            }
        }
    }

    



    cJSON *peers = cJSON_GetObjectItem(root, "Peers");
    if (!peers) {
        peers = cJSON_GetObjectItem(root, "PeersChanged");
        if (peers) ESP_LOGI(TAG, "Using 'PeersChanged' field for peer list");
    }
    if (!peers) {
        peers = cJSON_GetObjectItem(root, "peers");
        if (peers) ESP_LOGI(TAG, "Using 'peers' (lowercase) field for peer list");
    }
    if (!peers || !cJSON_IsArray(peers)) {
        
        goto check_removed;
    }

    int count = cJSON_GetArraySize(peers);
    ESP_LOGI(TAG, "MapResponse: %d peers", count);

    cJSON *peer;
    cJSON_ArrayForEach(peer, peers) {
        
        ml_peer_update_t *update = ml_psram_calloc(1, sizeof(ml_peer_update_t));
        if (!update) continue;

        update->action = ML_PEER_ADD;

        
        cJSON *name = cJSON_GetObjectItem(peer, "Name");
        if (name && name->valuestring) {
            strncpy(update->hostname, name->valuestring, sizeof(update->hostname) - 1);
            
            size_t hlen = strlen(update->hostname);
            if (hlen > 0 && update->hostname[hlen - 1] == '.') {
                update->hostname[hlen - 1] = '\0';
            }
        }

        
        cJSON *key = cJSON_GetObjectItem(peer, "Key");
        if (key && key->valuestring) {
            const char *hex = key->valuestring;
            if (strncmp(hex, "nodekey:", 8) == 0) hex += 8;
            hex_to_bytes(hex, update->public_key, 32);
        }

        
        cJSON *disco = cJSON_GetObjectItem(peer, "DiscoKey");
        if (disco && disco->valuestring) {
            const char *hex = disco->valuestring;
            if (strncmp(hex, "discokey:", 9) == 0) hex += 9;
            hex_to_bytes(hex, update->disco_key, 32);
        }

        

        cJSON *addresses = cJSON_GetObjectItem(peer, "Addresses");
        if (addresses && cJSON_IsArray(addresses)) {
            cJSON *addr_item;
            cJSON_ArrayForEach(addr_item, addresses) {
                if (!addr_item->valuestring) continue;
                unsigned a, b, c, d;
                
                if (sscanf(addr_item->valuestring, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
                    update->vpn_ip = (a << 24) | (b << 16) | (c << 8) | d;
                    update->is_ipv6 = false;
                    break;
                }
#if LWIP_IPV6
                if (!update->is_ipv6) {
                    char clean_v6[64];
                    strlcpy(clean_v6, addr_item->valuestring, sizeof(clean_v6));
                    char *slash = strchr(clean_v6, '/');
                    if (slash) *slash = '\0';
                    ip6_addr_t v6;
                    if (ip6addr_aton(clean_v6, &v6)) {
                        memcpy(&update->vpn_ipv6, &v6, sizeof(ip6_addr_t));
                        update->is_ipv6 = true;
                    }
                }
#endif
            }
        }

        
        bool key_is_zero = true;
        for (int ki = 0; ki < 32; ki++) {
            if (update->public_key[ki] != 0) { key_is_zero = false; break; }
        }
        
        if ((update->vpn_ip == 0 && !update->is_ipv6) || key_is_zero) {
            ESP_LOGD(TAG, "  Peer sin IP válida o sin clave descartado");
            free(update);
            continue;
        }

        
        if (update->hostname[0] == '\0') {
            if (update->is_ipv6) {
                snprintf(update->hostname, sizeof(update->hostname), "ingress-%02x%02x",
                         update->public_key[0], update->public_key[1]);
            } else {
                snprintf(update->hostname, sizeof(update->hostname), "peer-%02x%02x",
                         update->public_key[0], update->public_key[1]);
            }
        }

        
        cJSON *peer_home_derp = cJSON_GetObjectItem(peer, "HomeDERP");
        if (peer_home_derp && cJSON_IsNumber(peer_home_derp) && peer_home_derp->valueint > 0) {
            update->derp_region = (uint16_t)peer_home_derp->valueint;
        } else {
            cJSON *derp = cJSON_GetObjectItem(peer, "DERP");
            if (derp && derp->valuestring) {
                
                unsigned dr;
                if (sscanf(derp->valuestring, "127.3.3.40:%u", &dr) == 1) {
                    update->derp_region = (uint16_t)dr;
                }
            }
        }

        
        cJSON *endpoints = cJSON_GetObjectItem(peer, "Endpoints");
        if (endpoints) {
            int ep_count = 0;
            cJSON *ep;
            cJSON_ArrayForEach(ep, endpoints) {
                if (ep_count >= ML_MAX_ENDPOINTS) break;
                if (ep->valuestring) {
                    unsigned ea, eb, ec, ed, eport;
                    if (sscanf(ep->valuestring, "%u.%u.%u.%u:%u",
                               &ea, &eb, &ec, &ed, &eport) == 5) {
                        update->endpoints[ep_count].ip =
                            (ea << 24) | (eb << 16) | (ec << 8) | ed;
                        update->endpoints[ep_count].port = (uint16_t)eport;
                        update->endpoints[ep_count].is_ipv6 = false;
                        ep_count++;
                    }
                }
            }
            update->endpoint_count = ep_count;
        }

        if (update->is_ipv6) {
#if LWIP_IPV6
            char v6_str[48];
            ip6addr_ntoa_r(&update->vpn_ipv6, v6_str, sizeof(v6_str));
            ESP_LOGI(TAG, "  Ingress Peer: %s (%s) derp=%u",
                     update->hostname, v6_str, (unsigned)update->derp_region);
#endif
        } else {
            char ip_str[16];
            microlink_ip_to_str(update->vpn_ip, ip_str);
            ESP_LOGI(TAG, "  Peer: %s (%s) derp=%d eps=%d",
                     update->hostname, ip_str, update->derp_region, update->endpoint_count);
        }

        
        if (xQueueSend(ml->peer_update_queue, &update, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Peer update queue full, dropping %s", update->hostname);
            free(update);
        }
    }

check_removed:
    
    cJSON *removed = cJSON_GetObjectItem(root, "PeersRemoved");
    if (removed && cJSON_IsArray(removed)) {
        int rm_count = cJSON_GetArraySize(removed);
        ESP_LOGI(TAG, "PeersRemoved: %d peers", rm_count);

        cJSON *key_item;
        cJSON_ArrayForEach(key_item, removed) {
            if (!key_item->valuestring) continue;

            ml_peer_update_t *update = ml_psram_calloc(1, sizeof(ml_peer_update_t));
            if (!update) continue;

            update->action = ML_PEER_REMOVE;

            const char *hex = key_item->valuestring;
            if (strncmp(hex, "nodekey:", 8) == 0) hex += 8;
            hex_to_bytes(hex, update->public_key, 32);

            ESP_LOGI(TAG, "  Remove peer: %02x%02x%02x%02x...",
                     update->public_key[0], update->public_key[1],
                     update->public_key[2], update->public_key[3]);

            if (xQueueSend(ml->peer_update_queue, &update, pdMS_TO_TICKS(100)) != pdTRUE) {
                free(update);
            }
        }
    }

    
    cJSON *patches = cJSON_GetObjectItem(root, "PeersChangedPatch");
    if (patches && cJSON_IsObject(patches)) {
        ESP_LOGI(TAG, "PeersChangedPatch: processing lightweight updates");
        cJSON *patch = patches->child;
        while (patch) {
            if (patch->string && cJSON_IsObject(patch)) {
                
                ml_peer_update_t *update = ml_psram_calloc(1, sizeof(ml_peer_update_t));
                if (update) {
                    update->action = ML_PEER_UPDATE_ENDPOINT;

                    const char *hex = patch->string;
                    if (strncmp(hex, "nodekey:", 8) == 0) hex += 8;
                    hex_to_bytes(hex, update->public_key, 32);

                    

                    cJSON *patch_derp_region = cJSON_GetObjectItem(patch, "DERPRegion");
                    if (patch_derp_region && cJSON_IsNumber(patch_derp_region) && patch_derp_region->valueint > 0) {
                        update->derp_region = (uint16_t)patch_derp_region->valueint;
                    } else {
                        cJSON *derp = cJSON_GetObjectItem(patch, "DERP");
                        if (derp && derp->valuestring) {
                            unsigned dr;
                            if (sscanf(derp->valuestring, "127.3.3.40:%u", &dr) == 1) {
                                update->derp_region = (uint16_t)dr;
                            }
                        }
                    }

                    
                    cJSON *endpoints = cJSON_GetObjectItem(patch, "Endpoints");
                    if (endpoints) {
                        int ep_count = 0;
                        cJSON *ep;
                        cJSON_ArrayForEach(ep, endpoints) {
                            if (ep_count >= ML_MAX_ENDPOINTS) break;
                            if (ep->valuestring) {
                                unsigned ea, eb, ec, ed, eport;
                                if (sscanf(ep->valuestring, "%u.%u.%u.%u:%u",
                                           &ea, &eb, &ec, &ed, &eport) == 5) {
                                    update->endpoints[ep_count].ip =
                                        (ea << 24) | (eb << 16) | (ec << 8) | ed;
                                    update->endpoints[ep_count].port = (uint16_t)eport;
                                    update->endpoints[ep_count].is_ipv6 = false;
                                    ep_count++;
                                }
                            }
                        }
                        update->endpoint_count = ep_count;
                    }

                    ESP_LOGI(TAG, "  Patch peer: %02x%02x%02x%02x... eps=%d",
                             update->public_key[0], update->public_key[1],
                             update->public_key[2], update->public_key[3],
                             update->endpoint_count);

                    if (xQueueSend(ml->peer_update_queue, &update, pdMS_TO_TICKS(100)) != pdTRUE) {
                        free(update);
                    }
                }
            }
            patch = patch->next;
        }
    }
}




static int add_endpoints_to_json(microlink_t *ml, cJSON *root) {
    int count = 0;
    cJSON *ep_array = cJSON_AddArrayToObject(root, "Endpoints");
    cJSON *et_array = cJSON_AddArrayToObject(root, "EndpointTypes");
    if (!ep_array || !et_array) return 0;

    
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            uint32_t local_ip = ntohl(ip_info.ip.addr);
            char ep_str[32];
            snprintf(ep_str, sizeof(ep_str), "%lu.%lu.%lu.%lu:%u",
                     (unsigned long)((local_ip >> 24) & 0xFF),
                     (unsigned long)((local_ip >> 16) & 0xFF),
                     (unsigned long)((local_ip >> 8) & 0xFF),
                     (unsigned long)(local_ip & 0xFF),
                     ml->disco_local_port);
            cJSON_AddItemToArray(ep_array, cJSON_CreateString(ep_str));
            cJSON_AddItemToArray(et_array, cJSON_CreateNumber(1));  
            count++;
        }
    }

    
    

    
    if (ml->stun_public_ip != 0) {
        uint32_t pub_ip = ml->stun_public_ip;
        uint16_t pub_port = ml->stun_public_port ? ml->stun_public_port : ml->disco_local_port;
        char ep_str[32];
        snprintf(ep_str, sizeof(ep_str), "%lu.%lu.%lu.%lu:%u",
                 (unsigned long)((pub_ip >> 24) & 0xFF),
                 (unsigned long)((pub_ip >> 16) & 0xFF),
                 (unsigned long)((pub_ip >> 8) & 0xFF),
                 (unsigned long)(pub_ip & 0xFF),
                 pub_port);
        cJSON_AddItemToArray(ep_array, cJSON_CreateString(ep_str));
        cJSON_AddItemToArray(et_array, cJSON_CreateNumber(2));  
        count++;
    }

    

    if (ml->stun_has_ipv6) {
        const uint8_t *a = ml->stun_public_ip6;
        
        static const uint8_t v4mapped_prefix[12] = {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};
        bool is_v4mapped = (memcmp(a, v4mapped_prefix, 12) == 0);
        if (!is_v4mapped) {
            uint16_t port6 = ml->stun_public_port6 ? ml->stun_public_port6 : ml->disco_local_port;
            char ep6_str[64];
            snprintf(ep6_str, sizeof(ep6_str),
                     "[%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x]:%u",
                     a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                     a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15],
                     port6);
            cJSON_AddItemToArray(ep_array, cJSON_CreateString(ep6_str));
            cJSON_AddItemToArray(et_array, cJSON_CreateNumber(2));  
            count++;
        }
    }

    if (count > 0) {
        ESP_LOGI(TAG, "MapRequest includes %d endpoint(s)", count);
    }
    return count;
}

static int do_fetch_peers(microlink_t *ml, ml_noise_state_t *noise) {
    int64_t t_map_start = esp_timer_get_time();

    
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddNumberToObject(root, "Version", ML_CTRL_PROTOCOL_VER);

    char key_hex[65];
    char key_str[80];
    bytes_to_hex(ml->wg_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "nodekey:%s", key_hex);
    cJSON_AddStringToObject(root, "NodeKey", key_str);

    
    bytes_to_hex(ml->disco_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "discokey:%s", key_hex);
    cJSON_AddStringToObject(root, "DiscoKey", key_str);

    
    cJSON_AddBoolToObject(root, "Stream", false);
    cJSON_AddBoolToObject(root, "KeepAlive", true);
    cJSON_AddStringToObject(root, "Compress", "");  

    
    cJSON *hostinfo = cJSON_CreateObject();
    const char *dev_name = (ml->config.device_name && ml->config.device_name[0]) ? ml->config.device_name : microlink_default_device_name();
    cJSON_AddStringToObject(hostinfo, "Hostname", dev_name);
    cJSON_AddStringToObject(hostinfo, "OS", "linux");
    cJSON_AddStringToObject(hostinfo, "OSVersion", "ESP-IDF");
    cJSON_AddStringToObject(hostinfo, "GoArch", "arm");

    





    cJSON_AddStringToObject(hostinfo, "IPNVersion", MICROLINK_IPN_VERSION);
    cJSON_AddStringToObject(hostinfo, "App", MICROLINK_APP_NAME);

    

    add_funnel_hostinfo(ml, hostinfo);
    add_netinfo_to_hostinfo(ml, hostinfo);
    cJSON_AddItemToObject(root, "Hostinfo", hostinfo);

    

    add_endpoints_to_json(ml, root);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return -1;

    size_t json_len = strlen(json_str);
    ESP_LOGI(TAG, "MapRequest: %d bytes (Stream=false)", (int)json_len);

    
    uint8_t *h2_buf = ml_psram_malloc(json_len + 512);
    if (!h2_buf) { free(json_str); return -1; }

    int h2_pos = 0;

    int hdr_len = ml_h2_build_headers_frame(h2_buf + h2_pos, json_len + 512,
                                              "POST", "/machine/map",
                                              CTRL_HOST(ml), "application/json",
                                              3, false);
    if (hdr_len < 0) { free(json_str); free(h2_buf); return -1; }
    h2_pos += hdr_len;

    int data_len = ml_h2_build_data_frame(h2_buf + h2_pos, json_len + 512 - h2_pos,
                                            (uint8_t *)json_str, json_len,
                                            3, true);
    free(json_str);
    if (data_len < 0) { free(h2_buf); return -1; }
    h2_pos += data_len;

    if (noise_send(ml, noise, h2_buf, h2_pos) < 0) {
        free(h2_buf);
        return -1;
    }
    free(h2_buf);

    int64_t t_map_sent = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] MapRequest send: %lld ms", (t_map_sent - t_map_start) / 1000);

    



    uint8_t *h2_recv = ml_psram_malloc(ML_H2_BUFFER_SIZE);  
    if (!h2_recv) return -1;
    size_t h2_total = 0;

    uint8_t *resp_buf = ml_psram_malloc(ML_JSON_BUFFER_SIZE);
    if (!resp_buf) { free(h2_recv); return -1; }
    size_t json_total = 0;

    
    struct timeval rcv_tv = { .tv_sec = 60, .tv_usec = 0 };
    ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));

    uint64_t recv_start_ms = ml_get_time_ms();
    uint64_t last_progress_ms = recv_start_ms;
    size_t window_consumed = 0;

    



    bool got_end_stream = false;
    for (int read_count = 0; read_count < 200; read_count++) {
        uint8_t *frame_buf = ml_psram_malloc(65536);
        if (!frame_buf) break;

        int frame_len = noise_recv(ml, noise, frame_buf, 65536);
        if (frame_len <= 0) {
            free(frame_buf);
            break;
        }

        
        if (h2_total + frame_len < ML_H2_BUFFER_SIZE) {
            memcpy(h2_recv + h2_total, frame_buf, frame_len);
            h2_total += frame_len;
            window_consumed += frame_len;
        } else {
            ESP_LOGW(TAG, "H2 buffer full at %dKB, truncating", (int)(h2_total / 1024));
            free(frame_buf);
            break;
        }
        free(frame_buf);

        



        size_t scan_pos = 0;
        while (scan_pos + 9 <= h2_total) {
            uint32_t f_len = (h2_recv[scan_pos] << 16) | (h2_recv[scan_pos + 1] << 8) | h2_recv[scan_pos + 2];
            uint8_t f_type = h2_recv[scan_pos + 3];
            uint8_t f_flags = h2_recv[scan_pos + 4];

            if (scan_pos + 9 + f_len > h2_total) break;  

            if (f_type == 0x00 && (f_flags & 0x01)) {
                
                got_end_stream = true;
            }
            scan_pos += 9 + f_len;
        }

        if (got_end_stream) {
            ESP_LOGI(TAG, "H2 END_STREAM detected after %d Noise frames (%dKB, %lums)",
                     read_count + 1, (int)(h2_total / 1024),
                     (unsigned long)(ml_get_time_ms() - recv_start_ms));
            break;
        }

        
        uint64_t now = ml_get_time_ms();
        if (now - last_progress_ms > 5000) {
            ESP_LOGI(TAG, "MapResponse: received %dKB so far (%d frames, %lums elapsed)",
                     (int)(h2_total / 1024), read_count + 1,
                     (unsigned long)(now - recv_start_ms));
            last_progress_ms = now;
        }

        


        if (window_consumed >= 32768) {
            uint8_t wu_buf[26];  
            int wu_len = ml_h2_build_window_update(wu_buf, 13, 0, (uint32_t)window_consumed);
            wu_len += ml_h2_build_window_update(wu_buf + wu_len, 13, 3, (uint32_t)window_consumed);
            if (wu_len > 0) {
                noise_send(ml, noise, wu_buf, wu_len);
            }
            window_consumed = 0;
        }
    }

    
    rcv_tv.tv_sec = 5;
    ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));

    ESP_LOGI(TAG, "Accumulated %dKB of H2 data from Noise frames (%lums)",
             (int)(h2_total / 1024),
             (unsigned long)(ml_get_time_ms() - recv_start_ms));

    
    int fpos = 0;
    while (fpos + 9 <= (int)h2_total) {
        uint32_t f_len = (h2_recv[fpos] << 16) | (h2_recv[fpos + 1] << 8) | h2_recv[fpos + 2];
        uint8_t f_type = h2_recv[fpos + 3];
        uint8_t f_flags = h2_recv[fpos + 4];
        uint32_t f_stream = ((h2_recv[fpos + 5] & 0x7F) << 24) |
                            (h2_recv[fpos + 6] << 16) |
                            (h2_recv[fpos + 7] << 8) | h2_recv[fpos + 8];
        fpos += 9;

        ESP_LOGI(TAG, "  H2 frame: type=%d flags=0x%02x len=%lu stream=%lu",
                 f_type, f_flags, (unsigned long)f_len, (unsigned long)f_stream);

        if (fpos + (int)f_len > (int)h2_total) {
            ESP_LOGW(TAG, "  Incomplete H2 frame at end (need %lu, have %d)",
                     (unsigned long)f_len, (int)h2_total - fpos);
            break;
        }

        if (f_type == 0x00 && f_len > 0) {  
            if (json_total + f_len < ML_JSON_BUFFER_SIZE) {
                memcpy(resp_buf + json_total, h2_recv + fpos, f_len);
                json_total += f_len;
            }
        }

        fpos += f_len;
    }
    free(h2_recv);

    


    if (json_total > 0) {
        uint8_t wu_buf[13];
        int wu_len = ml_h2_build_window_update(wu_buf, 13, 0, (uint32_t)json_total);
        noise_send(ml, noise, wu_buf, wu_len);
        ESP_LOGI(TAG, "Sent H2 WINDOW_UPDATE: %d bytes (connection level)", (int)json_total);
    }

    if (json_total == 0) {
        ESP_LOGW(TAG, "Empty MapResponse");
        free(resp_buf);
        return -1;
    }

    ESP_LOGI(TAG, "MapResponse JSON: %d bytes", (int)json_total);

    
    {
        int dump = json_total < 32 ? (int)json_total : 32;
        char hexbuf[97];
        for (int i = 0; i < dump; i++) {
            sprintf(hexbuf + i * 3, "%02x ", resp_buf[i]);
        }
        hexbuf[dump * 3] = '\0';
        ESP_LOGI(TAG, "MapResponse first %d bytes (hex): %s", dump, hexbuf);
    }

    
    char *parse_start = (char *)resp_buf;
    size_t parse_len = json_total;

    
    int json_offset = -1;
    for (int i = 0; i < 8 && i < (int)json_total; i++) {
        if (resp_buf[i] == '{') {
            json_offset = i;
            break;
        }
    }
    if (json_offset > 0) {
        ESP_LOGI(TAG, "JSON starts at offset %d (skipping %d-byte prefix)", json_offset, json_offset);
        parse_start += json_offset;
        parse_len -= json_offset;
    } else if (json_offset < 0) {
        ESP_LOGW(TAG, "No '{' found in first 8 bytes of MapResponse!");
    }

    
    char saved = parse_start[parse_len];
    parse_start[parse_len] = '\0';

    cJSON *map_json = cJSON_Parse(parse_start);
    parse_start[parse_len] = saved;

    if (!map_json) {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "MapResponse JSON parse failed near: %.50s", err ? err : "unknown");
        free(resp_buf);
        return -1;
    }

    
    {
        ESP_LOGI(TAG, "MapResponse top-level fields:");
        cJSON *field = NULL;
        cJSON_ArrayForEach(field, map_json) {
            if (field->string) {
                if (cJSON_IsArray(field))
                    ESP_LOGI(TAG, "  - %s (array, size=%d)", field->string, cJSON_GetArraySize(field));
                else if (cJSON_IsObject(field))
                    ESP_LOGI(TAG, "  - %s (object)", field->string);
                else if (cJSON_IsString(field))
                    ESP_LOGI(TAG, "  - %s (string): %.40s", field->string, field->valuestring);
                else if (cJSON_IsNumber(field))
                    ESP_LOGI(TAG, "  - %s (number): %g", field->string, field->valuedouble);
                else if (cJSON_IsBool(field))
                    ESP_LOGI(TAG, "  - %s (bool): %s", field->string, cJSON_IsTrue(field) ? "true" : "false");
                else
                    ESP_LOGI(TAG, "  - %s (other)", field->string);
            }
        }
    }

    
    {
        cJSON *node = cJSON_GetObjectItem(map_json, "Node");
        if (node) {
            
            {
                cJSON *addresses = cJSON_GetObjectItem(node, "Addresses");
                parse_self_addresses(ml, addresses);
            }
            

            cJSON *home_derp = cJSON_GetObjectItem(node, "HomeDERP");
            if (home_derp && cJSON_IsNumber(home_derp) && home_derp->valueint > 0) {
                ml->derp_home_region = (uint16_t)home_derp->valueint;
                ESP_LOGI(TAG, "Home DERP region: %d (from server, HomeDERP)", ml->derp_home_region);
            } else {
                cJSON *self_derp = cJSON_GetObjectItem(node, "DERP");
                if (self_derp && self_derp->valuestring) {
                    ESP_LOGI(TAG, "Self-Node DERP: %s", self_derp->valuestring);
                    const char *colon = strrchr(self_derp->valuestring, ':');
                    if (colon) {
                        int region = atoi(colon + 1);
                        if (region > 0) {
                            ml->derp_home_region = (uint16_t)region;
                            ESP_LOGI(TAG, "Home DERP region: %d (from server, legacy DERP)", region);
                        }
                    }
                }
            }
            
            if (ml->derp_home_region == 0) {
                ml->derp_home_region = ML_DERP_REGION;
                ESP_LOGI(TAG, "Home DERP region: %d (default)", ML_DERP_REGION);
            }
            cJSON *self_key = cJSON_GetObjectItem(node, "Key");
            if (self_key && self_key->valuestring) {
                ESP_LOGI(TAG, "Self-Node Key (server): %.40s...", self_key->valuestring);
                char our_hex[65];
                bytes_to_hex(ml->wg_public_key, 32, our_hex);
                ESP_LOGI(TAG, "Our WG pubkey (local):  nodekey:%.32s...", our_hex);
            }

            
            cJSON *key_expiry = cJSON_GetObjectItem(node, "KeyExpiry");
            if (key_expiry && key_expiry->valuestring) {
                int yr, mo, dy, hr, mn, sc;
                if (sscanf(key_expiry->valuestring, "%d-%d-%dT%d:%d:%d",
                           &yr, &mo, &dy, &hr, &mn, &sc) >= 6) {
                    
                    
                    int64_t days = 0;
                    for (int y = 1970; y < yr; y++) {
                        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
                    }
                    static const int mdays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
                    for (int m = 1; m < mo; m++) {
                        days += mdays[m];
                        if (m == 2 && (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0))) days++;
                    }
                    days += dy - 1;
                    ml->key_expiry_epoch = days * 86400 + hr * 3600 + mn * 60 + sc;
                    ESP_LOGI(TAG, "Key expiry: %s (epoch: %lld)", key_expiry->valuestring,
                             (long long)ml->key_expiry_epoch);
                }
            }

            
            cJSON *expired = cJSON_GetObjectItem(node, "Expired");
            if (expired && cJSON_IsTrue(expired)) {
                ml->key_expired = true;
                ESP_LOGW(TAG, "Node key is EXPIRED — re-registration needed");
            } else {
                ml->key_expired = false;
            }
        }
    }

    
    parse_peers_from_map_response(ml, map_json);

    
    cJSON *derp_map = cJSON_GetObjectItem(map_json, "DERPMap");
    if (derp_map) {
        cJSON *regions = cJSON_GetObjectItem(derp_map, "Regions");
        if (regions) {
            ml->derp_region_count = 0;
            cJSON *region_obj;
            cJSON_ArrayForEach(region_obj, regions) {
                if (ml->derp_region_count >= ML_MAX_DERP_REGIONS) break;
                ml_derp_region_t *r = &ml->derp_regions[ml->derp_region_count];
                memset(r, 0, sizeof(*r));

                cJSON *rid = cJSON_GetObjectItem(region_obj, "RegionID");
                if (rid) r->region_id = (uint16_t)rid->valuedouble;

                cJSON *rcode = cJSON_GetObjectItem(region_obj, "RegionCode");
                if (rcode && rcode->valuestring) {
                    strncpy(r->code, rcode->valuestring, sizeof(r->code) - 1);
                }

                cJSON *avoid = cJSON_GetObjectItem(region_obj, "Avoid");
                if (avoid && cJSON_IsTrue(avoid)) r->avoid = true;

                
                cJSON *nodes = cJSON_GetObjectItem(region_obj, "Nodes");
                if (nodes) {
                    cJSON *node_obj;
                    cJSON_ArrayForEach(node_obj, nodes) {
                        if (r->node_count >= ML_MAX_DERP_NODES) break;
                        ml_derp_node_t *n = &r->nodes[r->node_count];
                        memset(n, 0, sizeof(*n));

                        cJSON *hn = cJSON_GetObjectItem(node_obj, "HostName");
                        if (hn && hn->valuestring) {
                            strncpy(n->hostname, hn->valuestring, sizeof(n->hostname) - 1);
                        }

                        cJSON *ip4 = cJSON_GetObjectItem(node_obj, "IPv4");
                        if (ip4 && ip4->valuestring) {
                            strncpy(n->ipv4, ip4->valuestring, sizeof(n->ipv4) - 1);
                        }

                        cJSON *ip6 = cJSON_GetObjectItem(node_obj, "IPv6");
                        if (ip6 && ip6->valuestring) {
                            strncpy(n->ipv6, ip6->valuestring, sizeof(n->ipv6) - 1);
                        }

                        cJSON *sp = cJSON_GetObjectItem(node_obj, "STUNPort");
                        if (sp) n->stun_port = (uint16_t)sp->valuedouble;

                        cJSON *dp = cJSON_GetObjectItem(node_obj, "DERPPort");
                        if (dp) n->derp_port = (uint16_t)dp->valuedouble;

                        cJSON *so = cJSON_GetObjectItem(node_obj, "STUNOnly");
                        if (so && cJSON_IsTrue(so)) n->stun_only = true;

                        r->node_count++;
                    }
                }

                ESP_LOGI(TAG, "  DERP region %d (%s): %d nodes%s",
                         r->region_id, r->code, r->node_count,
                         r->avoid ? " [avoid]" : "");
                for (int ni = 0; ni < r->node_count; ni++) {
                    ESP_LOGI(TAG, "    node: %s (v4=%s v6=%s stun=%d derp=%d%s)",
                             r->nodes[ni].hostname,
                             r->nodes[ni].ipv4[0] ? r->nodes[ni].ipv4 : "-",
                             r->nodes[ni].ipv6[0] ? r->nodes[ni].ipv6 : "-",
                             r->nodes[ni].stun_port ? r->nodes[ni].stun_port : 3478,
                             r->nodes[ni].derp_port ? r->nodes[ni].derp_port : 443,
                             r->nodes[ni].stun_only ? " stun-only" : "");
                }

                ml->derp_region_count++;
            }
            ESP_LOGI(TAG, "DERPMap: parsed %d regions", ml->derp_region_count);
        }
    }

    cJSON_Delete(map_json);
    free(resp_buf);

    int64_t t_map_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] MapResponse recv+parse: %lld ms (total map: %lld ms, %dKB)",
             (t_map_done - t_map_sent) / 1000,
             (t_map_done - t_map_start) / 1000,
             (int)(h2_total / 1024));

    return 0;
}






static int do_start_long_poll(microlink_t *ml, ml_noise_state_t *noise) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddNumberToObject(root, "Version", ML_CTRL_PROTOCOL_VER);

    char key_hex[65], key_str[80];
    bytes_to_hex(ml->wg_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "nodekey:%s", key_hex);
    cJSON_AddStringToObject(root, "NodeKey", key_str);

    bytes_to_hex(ml->disco_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "discokey:%s", key_hex);
    cJSON_AddStringToObject(root, "DiscoKey", key_str);

    

    cJSON *hostinfo = cJSON_CreateObject();
    if (hostinfo) {
        const char *dev_name = (ml->config.device_name && ml->config.device_name[0]) ? ml->config.device_name : microlink_default_device_name();
        cJSON_AddStringToObject(hostinfo, "Hostname", dev_name);
        cJSON_AddStringToObject(hostinfo, "OS", "linux");
        cJSON_AddStringToObject(hostinfo, "OSVersion", "ESP-IDF");
        cJSON_AddStringToObject(hostinfo, "GoArch", "arm");
        cJSON_AddStringToObject(hostinfo, "IPNVersion", MICROLINK_IPN_VERSION);
        cJSON_AddStringToObject(hostinfo, "App", MICROLINK_APP_NAME);
        add_funnel_hostinfo(ml, hostinfo);
        add_netinfo_to_hostinfo(ml, hostinfo);
        cJSON_AddItemToObject(root, "Hostinfo", hostinfo);
    }

    

    cJSON_AddBoolToObject(root, "Stream", true);
    cJSON_AddBoolToObject(root, "KeepAlive", true);
    cJSON_AddStringToObject(root, "Compress", "");    
    cJSON_AddBoolToObject(root, "OmitPeers", true);   

    



    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return -1;

    size_t json_len = strlen(json_str);
    ESP_LOGI(TAG, "MapRequest: %d bytes (Stream=true)", (int)json_len);

    
    uint8_t *h2_buf = ml_psram_malloc(json_len + 512);
    if (!h2_buf) { free(json_str); return -1; }

    int h2_pos = 0;
    int hdr_len = ml_h2_build_headers_frame(h2_buf, json_len + 512,
                                              "POST", "/machine/map",
                                              CTRL_HOST(ml), "application/json",
                                              5, false);
    if (hdr_len < 0) { free(json_str); free(h2_buf); return -1; }
    h2_pos += hdr_len;

    int data_len = ml_h2_build_data_frame(h2_buf + h2_pos, json_len + 512 - h2_pos,
                                            (uint8_t *)json_str, json_len,
                                            5, true);
    free(json_str);
    if (data_len < 0) { free(h2_buf); return -1; }
    h2_pos += data_len;

    if (noise_send(ml, noise, h2_buf, h2_pos) < 0) {
        free(h2_buf);
        return -1;
    }
    free(h2_buf);

    ESP_LOGI(TAG, "Streaming MapRequest sent on stream 5");
    return 0;
}









static int do_send_endpoint_update(microlink_t *ml, ml_noise_state_t *noise) {
    if (ml->stun_public_ip == 0 && !ml->stun_has_ipv6) {
        ESP_LOGD(TAG, "No STUN endpoints yet, skipping endpoint update");
        return 0;  
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddNumberToObject(root, "Version", ML_CTRL_PROTOCOL_VER);

    char key_hex[65], key_str[80];
    bytes_to_hex(ml->wg_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "nodekey:%s", key_hex);
    cJSON_AddStringToObject(root, "NodeKey", key_str);

    bytes_to_hex(ml->disco_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "discokey:%s", key_hex);
    cJSON_AddStringToObject(root, "DiscoKey", key_str);

    
    cJSON_AddBoolToObject(root, "Stream", false);
    cJSON_AddBoolToObject(root, "KeepAlive", true);
    
    cJSON_AddBoolToObject(root, "OmitPeers", true);
    cJSON_AddStringToObject(root, "Compress", "");

    
    cJSON *hostinfo = cJSON_CreateObject();
    if (hostinfo) {
        const char *dev_name = (ml->config.device_name && ml->config.device_name[0]) ? ml->config.device_name : microlink_default_device_name();
        cJSON_AddStringToObject(hostinfo, "Hostname", dev_name);
        cJSON_AddStringToObject(hostinfo, "OS", "linux");
        cJSON_AddStringToObject(hostinfo, "OSVersion", "ESP-IDF");
        cJSON_AddStringToObject(hostinfo, "GoArch", "arm");
        



        cJSON_AddStringToObject(hostinfo, "IPNVersion", MICROLINK_IPN_VERSION);
        cJSON_AddStringToObject(hostinfo, "App", MICROLINK_APP_NAME);
        add_funnel_hostinfo(ml, hostinfo);
        add_netinfo_to_hostinfo(ml, hostinfo);
        cJSON_AddItemToObject(root, "Hostinfo", hostinfo);
    }

    
    int ep_count = add_endpoints_to_json(ml, root);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return -1;

    size_t json_len = strlen(json_str);
    ESP_LOGI(TAG, "Endpoint update: %d bytes, %d endpoints (Stream=false, OmitPeers=true)",
             (int)json_len, ep_count);

    



    if (ml->h2_next_stream_id < 7) ml->h2_next_stream_id = 7;
    uint32_t sid = ml->h2_next_stream_id;
    ml->h2_next_stream_id += 2;

    uint8_t *h2_buf = ml_psram_malloc(json_len + 512);
    if (!h2_buf) { free(json_str); return -1; }

    int h2_pos = 0;
    int hdr_len = ml_h2_build_headers_frame(h2_buf, json_len + 512,
                                              "POST", "/machine/map",
                                              CTRL_HOST(ml), "application/json",
                                              sid, false);
    if (hdr_len < 0) { free(json_str); free(h2_buf); return -1; }
    h2_pos += hdr_len;

    int data_len = ml_h2_build_data_frame(h2_buf + h2_pos, json_len + 512 - h2_pos,
                                            (uint8_t *)json_str, json_len,
                                            sid, true);  
    free(json_str);
    if (data_len < 0) { free(h2_buf); return -1; }
    h2_pos += data_len;

    if (noise_send(ml, noise, h2_buf, h2_pos) < 0) {
        free(h2_buf);
        ESP_LOGE(TAG, "Failed to send endpoint update");
        return -1;
    }
    free(h2_buf);

    ESP_LOGI(TAG, "Endpoint update sent on H2 stream %lu", (unsigned long)sid);
    


    return 0;
}


static int poll_map_update(microlink_t *ml, ml_noise_state_t *noise) {
    
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(ml->coord_sock, &readfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };  
    int sel = ml_select_fds(ml->coord_sock + 1, &readfds, NULL, NULL, &tv);
    if (sel <= 0) return 0;  

    
    struct timeval tv_recv = { .tv_sec = 2, .tv_usec = 0 };
    ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv));

    uint8_t *frame_buf = ml_psram_malloc(65536);
    if (!frame_buf) return 0;

    int frame_len = noise_recv(ml, noise, frame_buf, 65536);

    if (frame_len <= 0) {
        free(frame_buf);
        int saved_errno = errno;
        
        if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) return 0;
        return frame_len;  
    }

    
    uint8_t *json_data = NULL;
    size_t json_data_len = 0;
    uint32_t total_data_bytes = 0;
    uint32_t data_stream_id = 0;
    int pos = 0;

    while (pos + 9 <= frame_len) {
        uint32_t f_len = (frame_buf[pos] << 16) | (frame_buf[pos + 1] << 8) | frame_buf[pos + 2];
        uint8_t f_type = frame_buf[pos + 3];
        uint8_t f_flags = frame_buf[pos + 4];
        uint32_t f_stream = ((frame_buf[pos + 5] & 0x7F) << 24) | (frame_buf[pos + 6] << 16) |
                             (frame_buf[pos + 7] << 8) | frame_buf[pos + 8];
        pos += 9;

        if (pos + (int)f_len > frame_len) break;

        if (f_type == 0x00) {  
            total_data_bytes += f_len;
            if (f_stream == 5) {
                
                data_stream_id = f_stream;
                if (f_len > 0) {
                    json_data = frame_buf + pos;
                    json_data_len = f_len;
                }
            } else if (f_len > 0) {
                
                ESP_LOGD(TAG, "H2 stream %lu DATA: %lu bytes (discarded)",
                         (unsigned long)f_stream, (unsigned long)f_len);
            }
        } else if (f_type == 0x06 && f_len == 8 && !(f_flags & 0x01)) {
            
            uint8_t pong[17];
            pong[0] = 0x00; pong[1] = 0x00; pong[2] = 0x08;
            pong[3] = 0x06; pong[4] = 0x01;
            pong[5] = 0x00; pong[6] = 0x00; pong[7] = 0x00; pong[8] = 0x00;
            memcpy(pong + 9, frame_buf + pos, 8);
            noise_send(ml, noise, pong, sizeof(pong));
            ESP_LOGI(TAG, "Sent HTTP/2 PONG in response to server PING");
        } else if (f_type == 0x04 && !(f_flags & 0x01)) {
            
            uint8_t settings_ack[9] = {0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00};
            noise_send(ml, noise, settings_ack, sizeof(settings_ack));
        }
        pos += f_len;
    }

    


    if (total_data_bytes > 0) {
        uint8_t wu_buf[26];  
        
        int wu_len = ml_h2_build_window_update(wu_buf, 13, 0, total_data_bytes);
        
        if (data_stream_id > 0) {
            wu_len += ml_h2_build_window_update(wu_buf + wu_len, 13,
                                                  data_stream_id, total_data_bytes);
        }
        noise_send(ml, noise, wu_buf, wu_len);
    }

    if (!json_data || json_data_len == 0) {
        
        free(frame_buf);
        return 1;  
    }

    
    char *parse_start = (char *)json_data;
    size_t parse_len = json_data_len;
    if (parse_len > 4 && parse_start[4] == '{') {
        parse_start += 4;
        parse_len -= 4;
    }

    char saved = parse_start[parse_len];
    parse_start[parse_len] = '\0';

    cJSON *update_json = cJSON_Parse(parse_start);
    parse_start[parse_len] = saved;

    if (update_json) {
        ESP_LOGI(TAG, "Long-poll MapResponse update received");

        
        cJSON *node = cJSON_GetObjectItem(update_json, "Node");
        if (node) {
            cJSON *addresses = cJSON_GetObjectItem(node, "Addresses");
            parse_self_addresses(ml, addresses);
        }

        
        parse_peers_from_map_response(ml, update_json);
        cJSON_Delete(update_json);
    }

    free(frame_buf);
    return 1;
}















typedef struct {
    char name[256];
    char value[136];
} ml_setdns_req_t;

typedef struct {
    esp_err_t err;
    char detail[96];
} ml_setdns_resp_t;

static QueueHandle_t s_setdns_req_q = NULL;
static QueueHandle_t s_setdns_resp_q = NULL;



static int do_set_dns(microlink_t *ml, ml_noise_state_t *noise,
                      const ml_setdns_req_t *req) {
    if (ml->coord_sock < 0) return -1;

    char key_hex[65];
    char key_str[80];
    bytes_to_hex(ml->wg_public_key, 32, key_hex);
    snprintf(key_str, sizeof(key_str), "nodekey:%s", key_hex);

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON_AddNumberToObject(root, "Version", ML_CTRL_PROTOCOL_VER);
    cJSON_AddStringToObject(root, "NodeKey", key_str);
    cJSON_AddStringToObject(root, "Type", "TXT");
    cJSON_AddStringToObject(root, "Name", req->name);
    cJSON_AddStringToObject(root, "Value", req->value);
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return -1;
    size_t json_len = strlen(json_str);

    if (ml->h2_next_stream_id < 7) ml->h2_next_stream_id = 7;
    uint32_t sid = ml->h2_next_stream_id;
    ml->h2_next_stream_id += 2;

    uint8_t *h2_buf = ml_psram_malloc(json_len + 512);
    if (!h2_buf) { free(json_str); return -1; }

    int h2_pos = 0;
    int hdr_len = ml_h2_build_headers_frame(h2_buf, json_len + 512,
                                              "POST", "/machine/set-dns",
                                              CTRL_HOST(ml), "application/json",
                                              sid, false);
    if (hdr_len < 0) { free(json_str); free(h2_buf); return -1; }
    h2_pos += hdr_len;

    int data_len = ml_h2_build_data_frame(h2_buf + h2_pos, json_len + 512 - h2_pos,
                                            (uint8_t *)json_str, json_len,
                                            sid, true);
    free(json_str);
    if (data_len < 0) { free(h2_buf); return -1; }
    h2_pos += data_len;

    ESP_LOGI(TAG, "SetDNS: publishing TXT %s (stream %lu)", req->name,
             (unsigned long)sid);
    if (noise_send(ml, noise, h2_buf, h2_pos) < 0) {
        free(h2_buf);
        return -1;
    }
    free(h2_buf);

    

    struct timeval short_tv = { .tv_sec = 3, .tv_usec = 0 };
    ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_RCVTIMEO, &short_tv, sizeof(short_tv));

    int found = 0;
    char detail[96] = {0};
    for (int i = 0; i < 8 && !found; i++) {
        uint8_t *frame_buf = ml_psram_malloc(4096);
        if (!frame_buf) break;
        int flen = noise_recv(ml, noise, frame_buf, 4096);
        if (flen <= 0) { free(frame_buf); continue; }

        



        size_t off = 0;
        while (off + 9 <= (size_t)flen) {
            uint8_t ftype = frame_buf[off + 3];
            uint8_t fflags = frame_buf[off + 4];
            uint16_t fl = ((uint16_t)frame_buf[off + 1] << 8) | frame_buf[off + 2];
            if (off + 9 + fl > (size_t)flen) break;
            uint32_t fsid = ((uint32_t)frame_buf[off + 5] << 24) | ((uint32_t)frame_buf[off + 6] << 16) |
                            ((uint32_t)frame_buf[off + 7] << 8) | (uint32_t)frame_buf[off + 8];

            if (fsid == sid) {
                if (ftype == 0x01 && fl >= 1 && frame_buf[off + 9] == 0x88) {
                    found = 1;
                    strlcpy(detail, "200", sizeof(detail));
                } else if (ftype == 0x00) {
                    if (fl > 0 && fl < sizeof(detail)) {
                        memcpy(detail, frame_buf + off + 9, fl);
                        detail[fl] = '\0';
                    }
                    if (fl == 0 || (fflags & 0x01)) {
                        found = 1;
                    }
                }
            }
            off += 9 + fl;
        }
        free(frame_buf);
    }

    struct timeval normal_tv = { .tv_sec = 5, .tv_usec = 0 };
    ml_setsockopt(ml->coord_sock, SOL_SOCKET, SO_RCVTIMEO, &normal_tv, sizeof(normal_tv));

    ESP_LOGI(TAG, "SetDNS response: %s (found=%d)", found ? detail : "<no data>", found);
    return found ? 0 : -1;
}


static void coord_process_setdns(microlink_t *ml, ml_noise_state_t *noise) {
    if (!s_setdns_req_q) return;
    ml_setdns_req_t req;
    if (xQueueReceive(s_setdns_req_q, &req, 0) != pdTRUE) return;

    ml_setdns_resp_t resp = { .err = ESP_FAIL, .detail = {0} };
    if (ml->state == ML_STATE_CONNECTED && do_set_dns(ml, noise, &req) == 0) {
        resp.err = ESP_OK;
        strlcpy(resp.detail, "txt published", sizeof(resp.detail));
    } else {
        strlcpy(resp.detail, "not connected or no response", sizeof(resp.detail));
    }
    if (s_setdns_resp_q) {
        xQueueSend(s_setdns_resp_q, &resp, 0);
    }
}





void ml_coord_task(void *arg) {
    microlink_t *ml = (microlink_t *)arg;
    ESP_LOGI(TAG, "Coord task started (Core %d)", xPortGetCoreID());

    coord_state_t state = COORD_IDLE;
    ml_coord_cmd_t cmd;
    uint64_t last_activity_ms = ml_get_time_ms();
    int reconnect_attempts = 0;

    
    ml_noise_state_t noise = {0};

    
    ESP_LOGI(TAG, "Waiting for WiFi...");
    {
        EventBits_t wb = xEventGroupWaitBits(ml->events,
                             ML_EVT_WIFI_CONNECTED | ML_EVT_SHUTDOWN_REQUEST,
                             pdFALSE, pdFALSE, portMAX_DELAY);
        if (wb & ML_EVT_SHUTDOWN_REQUEST) {
            ESP_LOGI(TAG, "Shutdown requested before WiFi, exiting");
            vTaskDelete(NULL);
            return;
        }
    }

    while (!(xEventGroupGetBits(ml->events) & ML_EVT_SHUTDOWN_REQUEST)) {
        
        if (xQueueReceive(ml->coord_cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd) {
            case ML_CMD_CONNECT:
                if (state == COORD_IDLE) {
                    state = COORD_STUN_PROBE;
                    ml->state = ML_STATE_CONNECTING;
                }
                break;
            case ML_CMD_DISCONNECT:
                if (ml->coord_sock >= 0) {
                    ml_close_sock(ml->coord_sock);
                    ml->coord_sock = -1;
                }
                state = COORD_IDLE;
                ml->state = ML_STATE_IDLE;
                break;
            case ML_CMD_FORCE_RECONNECT:
                state = COORD_RECONNECTING;
                break;
            case ML_CMD_UPDATE_ENDPOINTS:
                
                break;
            }
        }

        


        switch (state) {
        case COORD_IDLE:
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case COORD_STUN_PROBE:
            ESP_LOGI(TAG, "Running STUN probe...");
            
            ml_stun_resolve_servers(ml);
            
            ml->stun_retry_count = 1;  
            ml->stun_using_fallback = false;
            ml->stun_last_probe_ms = ml_get_time_ms();
            
            if (ml->stun_primary_ip) {
                ml_stun_send_probe_to(ml, ml->stun_primary_ip, ML_STUN_PRIMARY_PORT);
            } else if (ml->stun_fallback_ip) {
                ml->stun_using_fallback = true;
                ml_stun_send_probe_to(ml, ml->stun_fallback_ip, ML_STUN_FALLBACK_PORT);
            } else {
                
                ml_stun_send_probe(ml, ML_STUN_PRIMARY_HOST, ML_STUN_PRIMARY_PORT);
            }
            
            {
                static const uint8_t zero16[16] = {0};
                if (memcmp(ml->stun_primary_ip6, zero16, 16) != 0) {
                    ml_stun_send_probe_ipv6(ml, ml->stun_primary_ip6, ML_STUN_PRIMARY_PORT);
                }
            }
            state = COORD_DNS_RESOLVE;
            break;

        case COORD_DNS_RESOLVE:
        case COORD_TCP_CONNECT:
            if (do_tcp_connect(ml) < 0) {
                ESP_LOGE(TAG, "TCP connect failed, retrying...");
                state = COORD_RECONNECTING;
                break;
            }
            ml->h2_next_stream_id = 7;  
            state = COORD_NOISE_HANDSHAKE;
            break;

        case COORD_NOISE_HANDSHAKE:
            ml->state = ML_STATE_REGISTERING;
            if (do_noise_handshake(ml, &noise) < 0) {
                ESP_LOGE(TAG, "Noise handshake failed");
                ml_close_sock(ml->coord_sock);
                ml->coord_sock = -1;
                state = COORD_RECONNECTING;
                break;
            }
            


            process_proactive_frames(ml, &noise);
            state = COORD_H2_PREFACE;
            break;

        case COORD_H2_PREFACE:
            if (do_h2_preface(ml, &noise) < 0) {
                ESP_LOGE(TAG, "H2 preface failed");
                ml_close_sock(ml->coord_sock);
                ml->coord_sock = -1;
                state = COORD_RECONNECTING;
                break;
            }
            state = COORD_REGISTER;
            break;

        case COORD_REGISTER:
            ESP_LOGI(TAG, "Registering...");
            if (do_register(ml, &noise) < 0) {
                ESP_LOGE(TAG, "Registration failed");
                ml_close_sock(ml->coord_sock);
                ml->coord_sock = -1;
                state = COORD_RECONNECTING;
                break;
            }
            state = COORD_FETCH_PEERS;
            break;

        case COORD_FETCH_PEERS:
            ESP_LOGI(TAG, "Fetching peers...");
            if (do_fetch_peers(ml, &noise) < 0) {
                ESP_LOGW(TAG, "MapRequest failed, will retry");
                ml_close_sock(ml->coord_sock);
                ml->coord_sock = -1;
                state = COORD_RECONNECTING;
                break;
            }

            xEventGroupSetBits(ml->events, ML_EVT_COORD_REGISTERED);

            
            if (!ml->derp.connected) {
                xEventGroupSetBits(ml->events, ML_EVT_DERP_CONNECT_REQ);
                
                ESP_LOGI(TAG, "Waiting for DERP I/O task to connect...");
                xEventGroupWaitBits(ml->events, ML_EVT_DERP_CONNECTED,
                                    pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
            }

            
            if (do_start_long_poll(ml, &noise) < 0) {
                ESP_LOGW(TAG, "Failed to start long-poll (non-fatal)");
            }

            

            if (ml->stun_public_ip != 0 || ml->stun_has_ipv6) {
                do_send_endpoint_update(ml, &noise);
            }

            

            {
                uint8_t ping_frame[17];
                ping_frame[0] = 0x00; ping_frame[1] = 0x00; ping_frame[2] = 0x08;
                ping_frame[3] = 0x06; ping_frame[4] = 0x00;
                ping_frame[5] = 0x00; ping_frame[6] = 0x00;
                ping_frame[7] = 0x00; ping_frame[8] = 0x00;
                memset(ping_frame + 9, 0x42, 8);
                noise_send(ml, &noise, ping_frame, sizeof(ping_frame));
                ESP_LOGI(TAG, "Sent initial HTTP/2 PING after long-poll");
            }

            state = COORD_LONG_POLL;
            ml->state = ML_STATE_CONNECTED;
            reconnect_attempts = 0;
            last_activity_ms = ml_get_time_ms();

            
            if (ml->state_cb) {
                ml->state_cb(ml, ML_STATE_CONNECTED, ml->state_cb_data);
            }
            break;

        case COORD_LONG_POLL:
            {
                uint64_t now = ml_get_time_ms();

                
                if (now - last_activity_ms > ml->t_ctrl_watchdog_ms) {
                    ESP_LOGW(TAG, "Control plane watchdog timeout");
                    state = COORD_RECONNECTING;
                    break;
                }

                
                ml_rx_packet_t stun_pkt;
                if (xQueueReceive(ml->stun_rx_queue, &stun_pkt, 0) == pdTRUE) {
                    uint32_t pub_ip;
                    uint16_t pub_port;
                    bool parsed = false;

                    
                    if (ml_stun_parse_response(stun_pkt.data, stun_pkt.len,
                                                &pub_ip, &pub_port)) {
                        if (ml->stun_public_ip == 0) {
                            
                            ml->stun_public_ip = pub_ip;
                            ml->stun_public_port = pub_port;
                            ml->stun_retry_count = 0;
                            
                            if (!ml->stun_nat_checked && ml->stun_fallback_ip) {
                                ml_stun_send_probe_to(ml, ml->stun_fallback_ip,
                                                       ML_STUN_FALLBACK_PORT);
                                ESP_LOGI(TAG, "Sent STUN to fallback for NAT type detection");
                            }
                        } else if (!ml->stun_nat_checked) {
                            
                            ml->stun_secondary_port = pub_port;
                            ml->stun_nat_checked = true;
                            if (ml->stun_public_port != pub_port) {
                                ml->nat_mapping_varies = true;
                                ESP_LOGW(TAG, "Symmetric NAT detected: port %u vs %u "
                                         "(direct connections unlikely)",
                                         ml->stun_public_port, pub_port);
                            } else {
                                ml->nat_mapping_varies = false;
                                ESP_LOGI(TAG, "Cone NAT: port %u consistent "
                                         "(direct connections possible)", pub_port);
                            }
                        } else {
                            
                            ml->stun_public_ip = pub_ip;
                            ml->stun_public_port = pub_port;
                            ml->stun_retry_count = 0;
                        }
                        parsed = true;
                    }

                    
                    if (!parsed) {
                        uint8_t ip6[16];
                        uint16_t port6;
                        if (ml_stun_parse_response_ipv6(stun_pkt.data, stun_pkt.len,
                                                         ip6, &port6)) {
                            memcpy(ml->stun_public_ip6, ip6, 16);
                            ml->stun_public_port6 = port6;
                            ml->stun_has_ipv6 = true;
                            
                            if (ml->stun_public_port == 0) {
                                ml->stun_public_port = port6;
                            }
                            ml->stun_retry_count = 0;  
                            ESP_LOGI(TAG, "STUN IPv6 result obtained, port %u", port6);
                            
                            if (!ml->stun_nat_checked && ml->stun_fallback_ip) {
                                ml_stun_send_probe_to(ml, ml->stun_fallback_ip,
                                                       ML_STUN_FALLBACK_PORT);
                                ESP_LOGI(TAG, "Sent STUN to fallback for NAT type detection");
                            }
                            parsed = true;
                        }
                    }

                    if (parsed) {
                        xEventGroupSetBits(ml->events, ML_EVT_STUN_COMPLETE);
                        


                        do_send_endpoint_update(ml, &noise);
                    }
                    free(stun_pkt.data);
                }

                
                if (ml->stun_retry_count > 0 && ml->stun_retry_count <= ML_STUN_MAX_RETRIES) {
                    if (now - ml->stun_last_probe_ms > ML_STUN_RETRY_INTERVAL_MS) {
                        if (!ml->stun_using_fallback && ml->stun_primary_ip) {
                            ml_stun_send_probe_to(ml, ml->stun_primary_ip, ML_STUN_PRIMARY_PORT);
                        } else if (ml->stun_fallback_ip) {
                            ml_stun_send_probe_to(ml, ml->stun_fallback_ip, ML_STUN_FALLBACK_PORT);
                        }
                        ml->stun_last_probe_ms = now;
                        ml->stun_retry_count++;
                        if (ml->stun_retry_count > ML_STUN_MAX_RETRIES && !ml->stun_using_fallback) {
                            
                            ESP_LOGW(TAG, "STUN primary failed after %d retries, trying fallback",
                                     ML_STUN_MAX_RETRIES);
                            ml->stun_using_fallback = true;
                            ml->stun_retry_count = 1;
                            if (ml->stun_fallback_ip) {
                                ml_stun_send_probe_to(ml, ml->stun_fallback_ip, ML_STUN_FALLBACK_PORT);
                                ml->stun_last_probe_ms = now;
                            }
                        }
                    }
                }

                
                static uint64_t last_stun_ms = 0;
                if (now - last_stun_ms > ml->t_stun_interval_ms) {
                    ml->stun_retry_count = 1;
                    ml->stun_using_fallback = false;
                    ml->stun_last_probe_ms = now;
                    
                    if (ml->stun_primary_ip) {
                        ml_stun_send_probe_to(ml, ml->stun_primary_ip, ML_STUN_PRIMARY_PORT);
                    } else if (ml->stun_fallback_ip) {
                        ml->stun_using_fallback = true;
                        ml_stun_send_probe_to(ml, ml->stun_fallback_ip, ML_STUN_FALLBACK_PORT);
                    } else {
                        ml_stun_send_probe(ml, ML_STUN_PRIMARY_HOST, ML_STUN_PRIMARY_PORT);
                    }
                    
                    {
                        static const uint8_t zero16[16] = {0};
                        if (memcmp(ml->stun_primary_ip6, zero16, 16) != 0) {
                            ml_stun_send_probe_ipv6(ml, ml->stun_primary_ip6, ML_STUN_PRIMARY_PORT);
                        }
                    }
                    last_stun_ms = now;
                }

                
                static uint64_t last_derp_keepalive_ms = 0;
                if (ml->derp.connected && now - last_derp_keepalive_ms > 60000) {
                    uint8_t preferred = 0x01;
                    uint8_t *ka_data = malloc(1);
                    if (ka_data) {
                        *ka_data = preferred;
                        ml_derp_tx_item_t ka_item = {
                            .data = ka_data,
                            .len = 1,
                            .frame_type = 0x07,  
                        };
                        memset(ka_item.dest_pubkey, 0, 32);
                        if (xQueueSend(ml->derp_tx_queue, &ka_item, 0) != pdTRUE) {
                            free(ka_data);
                        }
                    }
                    last_derp_keepalive_ms = now;
                }

                
                if (ml->key_expiry_epoch > 0 || ml->key_expired) {
                    static uint64_t last_expiry_check_ms = 0;
                    if (now - last_expiry_check_ms > 60000) {
                        last_expiry_check_ms = now;
                        if (ml->key_expired) {
                            if (ml->config.auth_key) {
                                ESP_LOGW(TAG, "Key expired, re-registering with auth_key...");
                                state = COORD_RECONNECTING;
                                break;
                            } else {
                                ESP_LOGE(TAG, "Key expired but no auth_key — manual re-provisioning needed!");
                            }
                        }
                        
                        


                    }
                }

                


                static uint64_t last_h2_ping_ms = 0;
                if (now - last_h2_ping_ms >= 5000) {
                    uint8_t ping_frame[17];
                    ping_frame[0] = 0x00; ping_frame[1] = 0x00; ping_frame[2] = 0x08;
                    ping_frame[3] = 0x06;  
                    ping_frame[4] = 0x00;  
                    ping_frame[5] = 0x00; ping_frame[6] = 0x00;
                    ping_frame[7] = 0x00; ping_frame[8] = 0x00;  
                    
                    uint64_t ping_id = now;
                    for (int b = 0; b < 8; b++)
                        ping_frame[9 + b] = (ping_id >> (56 - b * 8)) & 0xFF;
                    int ping_ret = noise_send(ml, &noise, ping_frame, sizeof(ping_frame));
                    if (ping_ret >= 0) {
                        last_h2_ping_ms = now;
                        last_activity_ms = now;
                    } else {
                        ESP_LOGW(TAG, "H2 PING send failed, reconnecting");
                        state = COORD_RECONNECTING;
                        break;
                    }
                }

                
                coord_process_setdns(ml, &noise);

                
                int poll_ret = poll_map_update(ml, &noise);
                if (poll_ret > 0) {
                    last_activity_ms = now;  
                } else if (poll_ret < 0) {
                    ESP_LOGW(TAG, "Long-poll connection lost");
                    state = COORD_RECONNECTING;
                    break;
                }

                vTaskDelay(pdMS_TO_TICKS(50));
            }
            break;

        case COORD_RECONNECTING:
            {
                uint32_t backoff_ms = 1000 << (reconnect_attempts > 4 ? 4 : reconnect_attempts);
                if (backoff_ms > ML_CTRL_BACKOFF_MAX_MS) backoff_ms = ML_CTRL_BACKOFF_MAX_MS;

                ESP_LOGI(TAG, "Reconnecting in %lu ms (attempt %d)",
                         (unsigned long)backoff_ms, reconnect_attempts + 1);

                ml->state = ML_STATE_RECONNECTING;

                
                ml_coord_cmd_t wake_cmd;
                if (xQueueReceive(ml->coord_cmd_queue, &wake_cmd, pdMS_TO_TICKS(backoff_ms)) == pdTRUE) {
                    if (wake_cmd == ML_CMD_DISCONNECT) {
                        state = COORD_IDLE;
                        ml->state = ML_STATE_IDLE;
                        break;
                    }
                }

                reconnect_attempts++;

                
                if (ml->coord_sock >= 0) {
                    ml_close_sock(ml->coord_sock);
                    ml->coord_sock = -1;
                }

                
                memset(&noise, 0, sizeof(noise));

                state = COORD_STUN_PROBE;
            }
            break;
        }
    }

    
    if (ml->coord_sock >= 0) {
        ml_close_sock(ml->coord_sock);
        ml->coord_sock = -1;
    }
    memset(&noise, 0, sizeof(noise));

    ESP_LOGI(TAG, "Coord task exiting");
    vTaskDelete(NULL);
}





const char *microlink_get_fqdn(const microlink_t *ml) {
    if (!ml || ml->state != ML_STATE_CONNECTED || ml->self_fqdn[0] == '\0') {
        return NULL;
    }
    return ml->self_fqdn;
}

esp_err_t microlink_set_dns(const char *name, const char *value, uint32_t timeout_ms) {
    if (!name || !value || strlen(name) == 0 || strlen(name) >= 256 ||
        strlen(value) >= 136) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_setdns_req_q) {
        s_setdns_req_q = xQueueCreate(2, sizeof(ml_setdns_req_t));
        s_setdns_resp_q = xQueueCreate(2, sizeof(ml_setdns_resp_t));
        if (!s_setdns_req_q || !s_setdns_resp_q) return ESP_ERR_NO_MEM;
    }
    ml_setdns_req_t req = {0};
    strlcpy(req.name, name, sizeof(req.name));
    strlcpy(req.value, value, sizeof(req.value));
    if (xQueueSend(s_setdns_req_q, &req, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    ml_setdns_resp_t resp;
    if (xQueueReceive(s_setdns_resp_q, &resp, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return resp.err;
}
