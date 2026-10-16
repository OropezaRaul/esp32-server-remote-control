




















#include "microlink_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/error.h"
#include "nacl_box.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>

static const char *TAG = "ml_derp";


#define DERP_CONNECT_TIMEOUT_MS  10000














static int ml_derp_bio_recv_timeout(void *ctx, unsigned char *buf, size_t len,
                                      uint32_t timeout) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    


    uint32_t effective_timeout = (timeout > 0) ? timeout : DERP_CONNECT_TIMEOUT_MS;
    struct timeval tv;
    tv.tv_sec = effective_timeout / 1000;
    tv.tv_usec = (effective_timeout % 1000) * 1000;
    ml_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int ret = (int)ml_read_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return ret;
}




static int ml_derp_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    int ret = (int)ml_write_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}


#define DERP_FRAME_SERVER_KEY   0x01
#define DERP_FRAME_CLIENT_INFO  0x02
#define DERP_FRAME_SERVER_INFO  0x03
#define DERP_FRAME_SEND_PACKET  0x04
#define DERP_FRAME_RECV_PACKET  0x05
#define DERP_FRAME_KEEP_ALIVE   0x06
#define DERP_FRAME_NOTE_PREFERRED 0x07
#define DERP_FRAME_PEER_GONE    0x08
#define DERP_FRAME_PING         0x12
#define DERP_FRAME_PONG         0x13


static const uint8_t DISCO_MAGIC[6] = { 'T', 'S', 0xf0, 0x9f, 0x92, 0xac };









static int derp_tls_read_all(microlink_t *ml, uint8_t *data, size_t len, int timeout_ms) {
    size_t received = 0;
    uint64_t start_ms = ml_get_time_ms();

    while (received < len) {
        if (timeout_ms > 0 && (ml_get_time_ms() - start_ms) > (uint64_t)timeout_ms) {
            ESP_LOGW(TAG, "derp_tls_read_all timeout (%d/%d bytes in %dms)",
                     (int)received, (int)len, timeout_ms);
            return -2;
        }

        int ret = mbedtls_ssl_read(&ml->derp.ssl, data + received, len - received);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                ESP_LOGW(TAG, "DERP server closed connection");
                return -1;
            }
            ESP_LOGE(TAG, "TLS read failed: -0x%04x", -ret);
            return -1;
        }
        if (ret == 0) {
            ESP_LOGW(TAG, "TLS connection closed by peer (%d/%d bytes)",
                     (int)received, (int)len);
            return -1;
        }
        received += ret;
    }
    return (int)received;
}




static esp_err_t derp_recv_frame_header(microlink_t *ml, uint8_t *type,
                                          uint32_t *len, int timeout_ms) {
    uint8_t header[5];
    int ret = derp_tls_read_all(ml, header, 5, timeout_ms);
    if (ret < 0) {
        return (ret == -2) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    *type = header[0];
    *len = ((uint32_t)header[1] << 24) |
           ((uint32_t)header[2] << 16) |
           ((uint32_t)header[3] << 8) |
           (uint32_t)header[4];

    return ESP_OK;
}






static int derp_tls_write_all(microlink_t *ml, const uint8_t *data, size_t len) {
    size_t written = 0;
    int retries = 0;
    const int max_retries = 50;  

    while (written < len) {
        int ret = mbedtls_ssl_write(&ml->derp.ssl, data + written, len - written);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                if (++retries > max_retries) {
                    ESP_LOGW(TAG, "TLS write timeout after %d retries", retries);
                    return -1;
                }
                continue;
            }
            ESP_LOGE(TAG, "TLS write failed: -0x%04x", -ret);
            return -1;
        }
        written += ret;
        retries = 0;
    }
    return (int)written;
}


static int derp_write_frame(microlink_t *ml, uint8_t type,
                             const uint8_t *payload, uint32_t len) {
    
    uint8_t header[5];
    header[0] = type;
    header[1] = (len >> 24) & 0xFF;
    header[2] = (len >> 16) & 0xFF;
    header[3] = (len >> 8) & 0xFF;
    header[4] = len & 0xFF;

    if (derp_tls_write_all(ml, header, 5) < 0) return -1;

    if (len > 0 && payload) {
        if (derp_tls_write_all(ml, payload, len) < 0) return -1;
    }
    return 0;
}


static int derp_send_packet(microlink_t *ml, const uint8_t *dest_key,
                              const uint8_t *data, size_t len) {
    
    size_t frame_len = 32 + len;
    uint8_t *frame = malloc(frame_len);
    if (!frame) return -1;

    memcpy(frame, dest_key, 32);
    memcpy(frame + 32, data, len);

    int ret = derp_write_frame(ml, DERP_FRAME_SEND_PACKET, frame, frame_len);
    if (ret < 0) {
        ESP_LOGW(TAG, "derp_send_packet FAILED: dest=%02x%02x%02x%02x len=%d",
                 dest_key[0], dest_key[1], dest_key[2], dest_key[3], (int)len);
    }
    free(frame);
    return ret;
}






typedef enum {
    PKT_DISCO,
    PKT_STUN,
    PKT_WIREGUARD,
    PKT_UNKNOWN,
} pkt_type_t;

static pkt_type_t classify_packet(const uint8_t *data, size_t len) {
    if (len >= 20 && (data[0] == 0x00 || data[0] == 0x01) && data[1] == 0x01) {
        return PKT_STUN;
    }
    if (len >= 62 && memcmp(data, DISCO_MAGIC, 6) == 0) {
        return PKT_DISCO;
    }
    if (len >= 4) {
        return PKT_WIREGUARD;
    }
    return PKT_UNKNOWN;
}

static void route_derp_packet(microlink_t *ml, uint8_t *data, size_t len,
                               const uint8_t *src_pubkey) {
    pkt_type_t type = classify_packet(data, len);

    ml_rx_packet_t pkt = {
        .data = data,
        .len = len,
        .via_derp = true,
    };
    memcpy(pkt.src_pubkey, src_pubkey, 32);

    QueueHandle_t target = (type == PKT_DISCO) ? ml->disco_rx_queue : ml->wg_rx_queue;
    if (xQueueSend(target, &pkt, 0) != pdTRUE) {
        free(data);
    }
}


static void dispatch_derp_frame(microlink_t *ml, uint8_t frame_type,
                                 uint8_t *src_key, uint8_t *payload, size_t payload_len) {
    switch (frame_type) {
    case DERP_FRAME_RECV_PACKET:
        if (payload) {
            ESP_LOGI(TAG, "DERP RecvPacket: %d bytes from %02x%02x%02x%02x, hdr=%02x",
                     (int)payload_len,
                     src_key[0], src_key[1], src_key[2], src_key[3],
                     payload_len > 0 ? payload[0] : 0xFF);
            route_derp_packet(ml, payload, payload_len, src_key);
            return;  
        }
        break;

    case DERP_FRAME_KEEP_ALIVE:
        ESP_LOGD(TAG, "DERP KeepAlive received");
        break;

    case DERP_FRAME_PING:
        
        if (payload && payload_len > 0) {
            ESP_LOGD(TAG, "DERP PING received, sending PONG");
            derp_write_frame(ml, DERP_FRAME_PONG, payload, payload_len);
        }
        break;

    case DERP_FRAME_PONG:
        ESP_LOGD(TAG, "DERP PONG received");
        break;

    case DERP_FRAME_PEER_GONE:
        if (payload && payload_len >= 32) {
            ESP_LOGI(TAG, "DERP PeerGone: %02x%02x%02x%02x (len=%d)",
                     payload[0], payload[1], payload[2], payload[3],
                     (int)payload_len);
        }
        break;

    default:
        ESP_LOGD(TAG, "DERP frame type 0x%02x ignored (%d bytes)",
                 frame_type, (int)payload_len);
        break;
    }

    if (payload) free(payload);
}






static int poll_derp_read(microlink_t *ml) {
    if (!ml->derp.connected || ml->derp.sockfd < 0) return -1;

    

    uint8_t header[5];
    int n = mbedtls_ssl_read(&ml->derp.ssl, header, 5);
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
        n == MBEDTLS_ERR_SSL_TIMEOUT) {
        return 0;  
    }
    if (n <= 0) {
        ESP_LOGW(TAG, "DERP header read returned %d (0x%04x)", n, n < 0 ? -n : 0);
        return n;
    }
    if (n < 5) {
        ESP_LOGW(TAG, "DERP partial header: got %d of 5 bytes", n);
        return -1;
    }

    uint8_t frame_type = header[0];
    uint32_t len = (header[1] << 24) | (header[2] << 16) | (header[3] << 8) | header[4];

    uint8_t src_key[32] = {0};
    uint8_t *payload = NULL;
    size_t payload_len = 0;

    if (len == 0) {
        dispatch_derp_frame(ml, frame_type, src_key, NULL, 0);
        return 1;
    }

    if (len > 65536) {
        ESP_LOGW(TAG, "DERP frame too large: %lu", (unsigned long)len);
        return -1;
    }

    

    uint8_t *buf = ml_psram_malloc(len);
    if (!buf) return -1;

    size_t total_read = 0;
    uint64_t payload_start = ml_get_time_ms();
    while (total_read < len) {
        
        if (ml_get_time_ms() - payload_start > 5000) {
            ESP_LOGW(TAG, "DERP payload timeout at %d/%lu bytes",
                     (int)total_read, (unsigned long)len);
            free(buf);
            return -1;
        }
        n = mbedtls_ssl_read(&ml->derp.ssl, buf + total_read, len - total_read);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
            n == MBEDTLS_ERR_SSL_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (n <= 0) {
            ESP_LOGW(TAG, "DERP payload read error: %d (0x%04x) at %d/%lu bytes",
                     n, n < 0 ? -n : 0, (int)total_read, (unsigned long)len);
            free(buf);
            return n;
        }
        total_read += n;
    }

    
    if (frame_type == DERP_FRAME_RECV_PACKET && len > 32) {
        memcpy(src_key, buf, 32);
        payload = malloc(len - 32);
        if (payload) {
            memcpy(payload, buf + 32, len - 32);
            payload_len = len - 32;
        }
        free(buf);
    } else {
        payload = buf;
        payload_len = len;
    }

    dispatch_derp_frame(ml, frame_type, src_key, payload, payload_len);
    return 1;
}






esp_err_t ml_derp_queue_send(microlink_t *ml, const uint8_t *dest_key,
                              const uint8_t *data, size_t len) {
    if (!ml || !dest_key || !data || len == 0) return ESP_ERR_INVALID_ARG;

    uint8_t *pkt_data = malloc(len);
    if (!pkt_data) return ESP_ERR_NO_MEM;
    memcpy(pkt_data, data, len);

    ml_derp_tx_item_t item = {
        .data = pkt_data,
        .len = len,
        .frame_type = DERP_FRAME_SEND_PACKET,
    };
    memcpy(item.dest_pubkey, dest_key, 32);

    

    bool is_wg_handshake = (len >= 4 && (data[0] == 0x01 || data[0] == 0x02));

    
    if ((is_wg_handshake ? xQueueSendToFront(ml->derp_tx_queue, &item, 0)
                         : xQueueSend(ml->derp_tx_queue, &item, 0)) == pdTRUE) {
        return ESP_OK;
    }

    
    for (int i = 0; i < 3; i++) {
        ml_derp_tx_item_t dropped;
        if (xQueueReceive(ml->derp_tx_queue, &dropped, 0) == pdTRUE) {
            free(dropped.data);  
        }
        if (xQueueSend(ml->derp_tx_queue, &item, 0) == pdTRUE) {
            return ESP_OK;
        }
    }

    
    free(pkt_data);
    return ESP_ERR_TIMEOUT;
}





void ml_derp_tx_task(void *arg) {
    microlink_t *ml = (microlink_t *)arg;
    ESP_LOGI(TAG, "DERP I/O task started (Core %d)", xPortGetCoreID());

    uint32_t frames_rx = 0;
    uint32_t frames_tx = 0;
    uint64_t last_status_ms = 0;
    uint32_t loop_count = 0;
    uint64_t last_heartbeat_ms = 0;
    uint64_t connected_since_ms = 0;
    bool verbose_phase = false;  

    while (!(xEventGroupGetBits(ml->events) & ML_EVT_SHUTDOWN_REQUEST)) {
        loop_count++;
        uint64_t loop_start = ml_get_time_ms();

        
        if (loop_start - last_heartbeat_ms > 5000) {
            ESP_LOGW(TAG, "HEARTBEAT: loop=%lu conn=%d rx=%lu tx=%lu stack_free=%lu",
                     (unsigned long)loop_count, ml->derp.connected,
                     (unsigned long)frames_rx, (unsigned long)frames_tx,
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            last_heartbeat_ms = loop_start;
        }

        
        {
            uint64_t now_ms = loop_start;
            if (now_ms - last_status_ms > 10000) {
                ESP_LOGI(TAG, "DERP status: connected=%d fd=%d rx=%lu tx=%lu loops=%lu",
                         ml->derp.connected, ml->derp.sockfd,
                         (unsigned long)frames_rx, (unsigned long)frames_tx,
                         (unsigned long)loop_count);
                last_status_ms = now_ms;
            }
        }

        
        {
            EventBits_t bits = xEventGroupGetBits(ml->events);
            if ((bits & ML_EVT_DERP_CONNECT_REQ) && !ml->derp.connected) {
                xEventGroupClearBits(ml->events, ML_EVT_DERP_CONNECT_REQ);
                
                for (int attempt = 0; attempt < 3 && !ml->derp.connected; attempt++) {
                    if (attempt > 0) {
                        ESP_LOGW(TAG, "DERP connect retry %d/3 in 2s...", attempt + 1);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    } else {
                        ESP_LOGI(TAG, "DERP connect requested, connecting from I/O task");
                    }
                    if (ml_derp_connect(ml) == ESP_OK) {
                        connected_since_ms = ml_get_time_ms();
                        verbose_phase = true;
                        break;
                    }
                    ESP_LOGW(TAG, "DERP connect attempt %d failed", attempt + 1);
                }
            }
            if (bits & ML_EVT_DERP_RECONNECT) {
                xEventGroupClearBits(ml->events, ML_EVT_DERP_RECONNECT);
                ESP_LOGW(TAG, "DERP reconnect requested (was %s)",
                         ml->derp.connected ? "connected" : "disconnected");
                ml_derp_disconnect(ml);
                verbose_phase = false;
                
                vTaskDelay(pdMS_TO_TICKS(1000));
                for (int attempt = 0; attempt < 3 && !ml->derp.connected; attempt++) {
                    if (attempt > 0) {
                        ESP_LOGW(TAG, "DERP reconnect retry %d/3 in 2s...", attempt + 1);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }
                    if (ml_derp_connect(ml) == ESP_OK) {
                        connected_since_ms = ml_get_time_ms();
                        verbose_phase = true;
                        break;
                    }
                    ESP_LOGW(TAG, "DERP reconnect attempt %d failed", attempt + 1);
                }
            }
        }

        
        if (verbose_phase && ml_get_time_ms() - connected_since_ms > 15000) {
            verbose_phase = false;
        }

        if (!ml->derp.connected) {
            
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        
        {
            for (int tx_count = 0; tx_count < 8; tx_count++) {
                ml_derp_tx_item_t item;
                if (xQueueReceive(ml->derp_tx_queue, &item, 0) != pdTRUE) {
                    break;
                }
                if (!ml->derp.connected) {
                    free(item.data);
                    continue;
                }
                int ret;
                if (item.frame_type == DERP_FRAME_SEND_PACKET) {
                    ESP_LOGI(TAG, "DERP TX: SendPacket %d bytes, dest=%02x%02x%02x%02x, hdr=%02x",
                             (int)item.len, item.dest_pubkey[0], item.dest_pubkey[1],
                             item.dest_pubkey[2], item.dest_pubkey[3],
                             item.data[0]);
                    ret = derp_send_packet(ml, item.dest_pubkey, item.data, item.len);
                } else {
                    ret = derp_write_frame(ml, item.frame_type, item.data, item.len);
                }
                if (ret < 0) {
                    ESP_LOGW(TAG, "DERP write failed");
                    ml->derp.connected = false;
                    xEventGroupSetBits(ml->events, ML_EVT_DERP_RECONNECT);
                } else {
                    frames_tx++;
                }
                free(item.data);
            }
        }

        if (!ml->derp.connected) continue;

        
        {
            int burst;
            for (burst = 0; burst < 4; burst++) {
                int ret = poll_derp_read(ml);
                if (ret > 0) {
                    frames_rx++;
                    ml->derp.last_recv_ms = ml_get_time_ms();
                } else if (ret == 0) {
                    break;  
                } else {
                    ESP_LOGW(TAG, "DERP read error: %d", ret);
                    ml->derp.connected = false;
                    xEventGroupSetBits(ml->events, ML_EVT_DERP_RECONNECT);
                    break;
                }
            }
        }

        
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(TAG, "DERP I/O task exiting");
    vTaskDelete(NULL);
}





esp_err_t ml_derp_connect(microlink_t *ml) {
    



    const char *derp_host = ML_DERP_HOST;
    int derp_port = ML_DERP_PORT;

    if (ml->derp_region_count > 0 && ml->derp_home_region > 0) {
        for (int i = 0; i < ml->derp_region_count; i++) {
            if (ml->derp_regions[i].region_id == ml->derp_home_region) {
                

                for (int attempt = 0; attempt < ml->derp_regions[i].node_count; attempt++) {
                    if (!ml->derp_regions[i].nodes[attempt].stun_only &&
                        ml->derp_regions[i].nodes[attempt].hostname[0]) {
                        derp_host = ml->derp_regions[i].nodes[attempt].hostname;
                        if (ml->derp_regions[i].nodes[attempt].derp_port > 0) {
                            derp_port = ml->derp_regions[i].nodes[attempt].derp_port;
                        }
                        break;
                    }
                }
                break;
            }
        }
    }

    int64_t t_derp_start = esp_timer_get_time();

    ESP_LOGI(TAG, "Connecting to DERP %s:%d (region %d)",
             derp_host, derp_port, ml->derp_home_region ? ml->derp_home_region : ML_DERP_REGION);

    
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", derp_port);

    if (ml_getaddrinfo(derp_host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolve failed for %s", derp_host);
        return ESP_FAIL;
    }

    int64_t t_derp_dns = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP DNS: %lld ms", (t_derp_dns - t_derp_start) / 1000);

    
    int sock = ml_socket(res->ai_family, SOCK_STREAM, 0);
    if (sock < 0) {
        ml_freeaddrinfo(res);
        return ESP_FAIL;
    }

    
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (ml_connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        ESP_LOGE(TAG, "TCP connect failed: %d", errno);
        ml_close_sock(sock);
        ml_freeaddrinfo(res);
        return ESP_FAIL;
    }
    ml_freeaddrinfo(res);

    int64_t t_derp_tcp = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP TCP connect: %lld ms", (t_derp_tcp - t_derp_dns) / 1000);

    
    mbedtls_ssl_init(&ml->derp.ssl);
    mbedtls_ssl_config_init(&ml->derp.ssl_conf);
    mbedtls_entropy_init(&ml->derp.entropy);
    mbedtls_ctr_drbg_init(&ml->derp.ctr_drbg);

    mbedtls_ctr_drbg_seed(&ml->derp.ctr_drbg, mbedtls_entropy_func,
                           &ml->derp.entropy, NULL, 0);

    mbedtls_ssl_config_defaults(&ml->derp.ssl_conf,
                                 MBEDTLS_SSL_IS_CLIENT,
                                 MBEDTLS_SSL_TRANSPORT_STREAM,
                                 MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&ml->derp.ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&ml->derp.ssl_conf, mbedtls_ctr_drbg_random, &ml->derp.ctr_drbg);
    mbedtls_ssl_conf_read_timeout(&ml->derp.ssl_conf, DERP_CONNECT_TIMEOUT_MS);

    mbedtls_ssl_setup(&ml->derp.ssl, &ml->derp.ssl_conf);
    mbedtls_ssl_set_hostname(&ml->derp.ssl, derp_host);
    



    ml->derp.sockfd = sock;
    mbedtls_ssl_set_bio(&ml->derp.ssl, &ml->derp.sockfd,
                         ml_derp_bio_send, NULL, ml_derp_bio_recv_timeout);

    
    int ret;
    while ((ret = mbedtls_ssl_handshake(&ml->derp.ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        char err_buf[128];
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGE(TAG, "TLS handshake failed: %s", err_buf);
        ml_close_sock(sock);
        ml->derp.sockfd = -1;
        return ESP_FAIL;
    }

    int64_t t_derp_tls = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP TLS handshake: %lld ms", (t_derp_tls - t_derp_tcp) / 1000);
    ESP_LOGI(TAG, "TLS connected to DERP");

    
    char upgrade_req[256];
    snprintf(upgrade_req, sizeof(upgrade_req),
             "GET /derp HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: Upgrade\r\n"
             "Upgrade: DERP\r\n"
             "\r\n",
             derp_host);

    ret = mbedtls_ssl_write(&ml->derp.ssl, (const uint8_t *)upgrade_req, strlen(upgrade_req));
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send HTTP upgrade");
        ml_close_sock(sock);
        ml->derp.sockfd = -1;
        return ESP_FAIL;
    }

    

    {
        uint8_t resp_buf[512];
        int resp_len = 0;
        bool found_end = false;
        uint64_t http_start = ml_get_time_ms();

        while (resp_len < (int)sizeof(resp_buf) - 1) {
            if (ml_get_time_ms() - http_start > DERP_CONNECT_TIMEOUT_MS) {
                ESP_LOGE(TAG, "HTTP upgrade response timeout");
                ml_close_sock(sock);
                ml->derp.sockfd = -1;
                return ESP_FAIL;
            }

            ret = mbedtls_ssl_read(&ml->derp.ssl, resp_buf + resp_len, 1);
            if (ret < 0) {
                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                    ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                ESP_LOGE(TAG, "HTTP upgrade read failed: -0x%04x", -ret);
                ml_close_sock(sock);
                ml->derp.sockfd = -1;
                return ESP_FAIL;
            }
            if (ret == 0) {
                ESP_LOGE(TAG, "Connection closed during HTTP upgrade");
                ml_close_sock(sock);
                ml->derp.sockfd = -1;
                return ESP_FAIL;
            }
            resp_len++;

            
            if (resp_len >= 4 &&
                resp_buf[resp_len - 4] == '\r' && resp_buf[resp_len - 3] == '\n' &&
                resp_buf[resp_len - 2] == '\r' && resp_buf[resp_len - 1] == '\n') {
                found_end = true;
                break;
            }
        }

        resp_buf[resp_len] = '\0';

        if (!found_end || strstr((char *)resp_buf, "101") == NULL) {
            ESP_LOGE(TAG, "DERP upgrade rejected: %.100s", resp_buf);
            ml_close_sock(sock);
            ml->derp.sockfd = -1;
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "HTTP 101 Switching Protocols received");
    }

    



    {
        int flags = ml_fcntl(sock, F_GETFL, 0);
        if (flags >= 0) {
            ml_fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        }
        struct timeval io_tv = { .tv_sec = 0, .tv_usec = 100000 };  
        ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
        ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));
    }

    



    
    uint8_t frame_type;
    uint32_t frame_len;
    esp_err_t err = derp_recv_frame_header(ml, &frame_type, &frame_len, DERP_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read ServerKey frame header (err=%d)", err);
        ml_close_sock(sock);
        ml->derp.sockfd = -1;
        return ESP_FAIL;
    }

    if (frame_type != DERP_FRAME_SERVER_KEY || frame_len < 40) {
        ESP_LOGE(TAG, "Expected ServerKey frame (0x01), got 0x%02x len=%lu",
                 frame_type, (unsigned long)frame_len);
        ml_close_sock(sock);
        ml->derp.sockfd = -1;
        return ESP_FAIL;
    }

    
    uint8_t magic[8];
    static const uint8_t DERP_MAGIC[8] = {0x44, 0x45, 0x52, 0x50, 0xf0, 0x9f, 0x94, 0x91};
    if (derp_tls_read_all(ml, magic, 8, DERP_CONNECT_TIMEOUT_MS) < 0) {
        ESP_LOGE(TAG, "Failed to read ServerKey magic");
        ml_close_sock(sock);
        ml->derp.sockfd = -1;
        return ESP_FAIL;
    }

    if (memcmp(magic, DERP_MAGIC, 8) != 0) {
        ESP_LOGE(TAG, "Invalid DERP magic: %02x%02x%02x%02x%02x%02x%02x%02x",
                 magic[0], magic[1], magic[2], magic[3],
                 magic[4], magic[5], magic[6], magic[7]);
        ml_close_sock(sock);
        ml->derp.sockfd = -1;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "DERP magic verified");

    
    uint8_t derp_server_key[32];
    if (derp_tls_read_all(ml, derp_server_key, 32, DERP_CONNECT_TIMEOUT_MS) < 0) {
        ESP_LOGE(TAG, "Failed to read server key");
        ml_close_sock(sock);
        ml->derp.sockfd = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "DERP server key received (first 8): %02x%02x%02x%02x%02x%02x%02x%02x",
             derp_server_key[0], derp_server_key[1], derp_server_key[2], derp_server_key[3],
             derp_server_key[4], derp_server_key[5], derp_server_key[6], derp_server_key[7]);

    
    if (frame_len > 40) {
        uint8_t skip_buf[64];
        size_t remaining = frame_len - 40;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(skip_buf) ? sizeof(skip_buf) : remaining;
            if (derp_tls_read_all(ml, skip_buf, chunk, DERP_CONNECT_TIMEOUT_MS) < 0) break;
            remaining -= chunk;
        }
    }

    

    {
        const char *client_info_json = "{\"Version\":2,\"CanAckPings\":true,\"IsProber\":false}";
        size_t json_len = strlen(client_info_json);

        
        uint8_t nonce[NACL_BOX_NONCEBYTES];
        esp_fill_random(nonce, NACL_BOX_NONCEBYTES);

        
        size_t ciphertext_len = json_len + NACL_BOX_MACBYTES;
        uint8_t *ciphertext = malloc(ciphertext_len);
        if (!ciphertext) {
            ml_close_sock(sock);
            ml->derp.sockfd = -1;
            return ESP_FAIL;
        }

        if (nacl_box(ciphertext,
                     (const uint8_t *)client_info_json, json_len,
                     nonce,
                     derp_server_key,       
                     ml->wg_private_key     
                     ) != 0) {
            ESP_LOGE(TAG, "NaCl box encrypt failed");
            free(ciphertext);
            ml_close_sock(sock);
            ml->derp.sockfd = -1;
            return ESP_FAIL;
        }

        
        size_t ci_payload_len = 32 + NACL_BOX_NONCEBYTES + ciphertext_len;
        uint8_t *ci_payload = malloc(ci_payload_len);
        if (!ci_payload) {
            free(ciphertext);
            ml_close_sock(sock);
            ml->derp.sockfd = -1;
            return ESP_FAIL;
        }

        memcpy(ci_payload, ml->wg_public_key, 32);
        memcpy(ci_payload + 32, nonce, NACL_BOX_NONCEBYTES);
        memcpy(ci_payload + 32 + NACL_BOX_NONCEBYTES, ciphertext, ciphertext_len);
        free(ciphertext);

        ESP_LOGI(TAG, "DERP ClientInfo node_key=%02x%02x%02x%02x%02x%02x%02x%02x",
                 ml->wg_public_key[0], ml->wg_public_key[1],
                 ml->wg_public_key[2], ml->wg_public_key[3],
                 ml->wg_public_key[4], ml->wg_public_key[5],
                 ml->wg_public_key[6], ml->wg_public_key[7]);

        
        if (derp_write_frame(ml, DERP_FRAME_CLIENT_INFO, ci_payload, ci_payload_len) < 0) {
            ESP_LOGE(TAG, "Failed to send ClientInfo");
            free(ci_payload);
            ml_close_sock(sock);
            ml->derp.sockfd = -1;
            return ESP_FAIL;
        }
        free(ci_payload);

        ESP_LOGI(TAG, "ClientInfo sent");
    }

    
    {
        uint8_t si_type;
        uint32_t si_len;
        err = derp_recv_frame_header(ml, &si_type, &si_len, DERP_CONNECT_TIMEOUT_MS);
        if (err == ESP_OK && si_type == DERP_FRAME_SERVER_INFO && si_len > 0) {
            
            uint8_t *si_buf = malloc(si_len);
            if (si_buf) {
                derp_tls_read_all(ml, si_buf, si_len, DERP_CONNECT_TIMEOUT_MS);
                free(si_buf);
            }
            ESP_LOGI(TAG, "ServerInfo received (discarded)");
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "No ServerInfo frame (continuing anyway)");
        }
    }

    
    {
        uint8_t preferred = 0x01;
        derp_write_frame(ml, DERP_FRAME_NOTE_PREFERRED, &preferred, 1);
    }

    

    {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };  
        ml_setsockopt(ml->derp.sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        mbedtls_ssl_conf_read_timeout(&ml->derp.ssl_conf, 200);
    }

    ml->derp.connected = true;
    ml->derp.last_recv_ms = ml_get_time_ms();
    xEventGroupSetBits(ml->events, ML_EVT_DERP_CONNECTED);

    int64_t t_derp_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP total: %lld ms (DNS=%lld, TCP=%lld, TLS=%lld, proto=%lld)",
             (t_derp_done - t_derp_start) / 1000,
             (t_derp_dns - t_derp_start) / 1000,
             (t_derp_tcp - t_derp_dns) / 1000,
             (t_derp_tls - t_derp_tcp) / 1000,
             (t_derp_done - t_derp_tls) / 1000);
    ESP_LOGI(TAG, "DERP handshake complete, connected");
    return ESP_OK;
}

void ml_derp_disconnect(microlink_t *ml) {
    ml->derp.connected = false;
    xEventGroupClearBits(ml->events, ML_EVT_DERP_CONNECTED);

    if (ml->derp.sockfd >= 0) {
        mbedtls_ssl_close_notify(&ml->derp.ssl);
        mbedtls_ssl_free(&ml->derp.ssl);
        mbedtls_ssl_config_free(&ml->derp.ssl_conf);
        mbedtls_ctr_drbg_free(&ml->derp.ctr_drbg);
        mbedtls_entropy_free(&ml->derp.entropy);
        ml_close_sock(ml->derp.sockfd);
        ml->derp.sockfd = -1;
    }

    
    ml_derp_tx_item_t item;
    while (xQueueReceive(ml->derp_tx_queue, &item, 0) == pdTRUE) {
        free(item.data);
    }

    ESP_LOGI(TAG, "DERP disconnected");
}
