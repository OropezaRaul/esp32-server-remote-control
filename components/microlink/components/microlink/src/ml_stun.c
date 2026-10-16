














#include "microlink_internal.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_crc.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "ml_stun";


#define STUN_MAGIC_COOKIE               0x2112A442
#define STUN_HEADER_SIZE                20
#define STUN_BINDING_REQUEST            0x0001
#define STUN_BINDING_RESPONSE           0x0101
#define STUN_ATTR_XOR_MAPPED_ADDRESS    0x0020
#define STUN_ATTR_MAPPED_ADDRESS        0x0001
#define STUN_ATTR_SOFTWARE              0x8022
#define STUN_ATTR_FINGERPRINT           0x8028
#define STUN_FINGERPRINT_XOR            0x5354554e  


static const char STUN_SOFTWARE[] = "tailnode";
#define STUN_SOFTWARE_LEN   8


#define STUN_REQUEST_SIZE   40


#define STUN_ATTR_SOFTWARE_TOTAL    (4 + STUN_SOFTWARE_LEN)  
#define STUN_ATTR_FINGERPRINT_TOTAL 8                         


static esp_err_t ensure_stun_socket6(microlink_t *ml);


static uint8_t txid_v4[12];
static bool txid_v4_valid = false;
static uint8_t txid_v6[12];
static bool txid_v6_valid = false;





static uint32_t stun_fingerprint(const uint8_t *data, size_t len) {
    uint32_t crc = esp_crc32_le(0, data, len);
    return crc ^ STUN_FINGERPRINT_XOR;
}





static size_t build_stun_request(uint8_t *out, uint8_t *txid_out) {
    size_t pos = 0;

    
    out[pos++] = 0x00;
    out[pos++] = 0x01;

    
    uint16_t msg_len = STUN_ATTR_SOFTWARE_TOTAL + STUN_ATTR_FINGERPRINT_TOTAL;
    out[pos++] = (msg_len >> 8) & 0xFF;
    out[pos++] = msg_len & 0xFF;

    
    out[pos++] = 0x21;
    out[pos++] = 0x12;
    out[pos++] = 0xA4;
    out[pos++] = 0x42;

    
    esp_fill_random(txid_out, 12);
    memcpy(out + pos, txid_out, 12);
    pos += 12;

    
    out[pos++] = (STUN_ATTR_SOFTWARE >> 8) & 0xFF;
    out[pos++] = STUN_ATTR_SOFTWARE & 0xFF;
    out[pos++] = 0x00;
    out[pos++] = STUN_SOFTWARE_LEN;
    memcpy(out + pos, STUN_SOFTWARE, STUN_SOFTWARE_LEN);
    pos += STUN_SOFTWARE_LEN;

    
    uint32_t fp = stun_fingerprint(out, pos);
    out[pos++] = (STUN_ATTR_FINGERPRINT >> 8) & 0xFF;
    out[pos++] = STUN_ATTR_FINGERPRINT & 0xFF;
    out[pos++] = 0x00;
    out[pos++] = 0x04;
    out[pos++] = (fp >> 24) & 0xFF;
    out[pos++] = (fp >> 16) & 0xFF;
    out[pos++] = (fp >> 8) & 0xFF;
    out[pos++] = fp & 0xFF;

    return pos;  
}





static esp_err_t ensure_stun_socket(microlink_t *ml) {
    if (ml->stun_sock >= 0) return ESP_OK;

    ml->stun_sock = ml_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ml->stun_sock < 0) {
        ESP_LOGE(TAG, "Failed to create STUN socket: %d", errno);
        return ESP_FAIL;
    }

    
    int flags = ml_fcntl(ml->stun_sock, F_GETFL, 0);
    ml_fcntl(ml->stun_sock, F_SETFL, flags | O_NONBLOCK);

    
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = INADDR_ANY,
    };
    ml_bind(ml->stun_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    return ESP_OK;
}





esp_err_t ml_stun_resolve_servers(microlink_t *ml) {
    struct addrinfo *res = NULL;

    
    const char *primary_host = ML_STUN_PRIMARY_HOST;
    uint16_t primary_port = ML_STUN_PRIMARY_PORT;

    if (ml->derp_region_count > 0) {
        
        uint16_t target_region = ml->derp_home_region ? ml->derp_home_region : ML_DERP_REGION;
        for (int i = 0; i < ml->derp_region_count; i++) {
            if (ml->derp_regions[i].region_id == target_region &&
                ml->derp_regions[i].node_count > 0 &&
                ml->derp_regions[i].nodes[0].hostname[0]) {
                primary_host = ml->derp_regions[i].nodes[0].hostname;
                if (ml->derp_regions[i].nodes[0].stun_port > 0) {
                    primary_port = ml->derp_regions[i].nodes[0].stun_port;
                }
                
                if (ml->stun_primary_ip == 0 && ml->derp_regions[i].nodes[0].ipv4[0]) {
                    unsigned a, b, c, d;
                    if (sscanf(ml->derp_regions[i].nodes[0].ipv4, "%u.%u.%u.%u",
                               &a, &b, &c, &d) == 4) {
                        ml->stun_primary_ip = (a << 24) | (b << 16) | (c << 8) | d;
                        char ip_str[16];
                        microlink_ip_to_str(ml->stun_primary_ip, ip_str);
                        ESP_LOGI(TAG, "STUN primary from DERPMap: %s:%u (region %d)",
                                 ip_str, primary_port, target_region);
                    }
                }
                break;
            }
        }
    }

    
    static const uint8_t zero16[16] = {0};
    if (ml->stun_primary_ip == 0 && memcmp(ml->stun_primary_ip6, zero16, 16) == 0) {
        char port_str[6];
        snprintf(port_str, sizeof(port_str), "%u", primary_port);
        
        struct addrinfo hints_any = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM };
        if (ml_getaddrinfo(primary_host, port_str, &hints_any, &res) == 0 && res) {
            if (res->ai_family == AF_INET) {
                struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
                ml->stun_primary_ip = ntohl(addr->sin_addr.s_addr);
                char ip_str[16];
                microlink_ip_to_str(ml->stun_primary_ip, ip_str);
                ESP_LOGI(TAG, "STUN primary resolved: %s -> %s", primary_host, ip_str);
            } else if (res->ai_family == AF_INET6) {
                struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)res->ai_addr;
                memcpy(ml->stun_primary_ip6, &addr6->sin6_addr, 16);
                ESP_LOGI(TAG, "STUN primary resolved: %s -> IPv6 (carrier IPv6-only)",
                         primary_host);
            }
            ml_freeaddrinfo(res);
            res = NULL;
        } else {
            ESP_LOGW(TAG, "DNS resolve failed for %s", primary_host);
            if (res) ml_freeaddrinfo(res);
            res = NULL;
        }
    }

    
    if (ml->stun_fallback_ip == 0) {
        struct addrinfo hints_any = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM };
        if (ml_getaddrinfo(ML_STUN_FALLBACK_HOST, "19302", &hints_any, &res) == 0 && res) {
            if (res->ai_family == AF_INET) {
                struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
                ml->stun_fallback_ip = ntohl(addr->sin_addr.s_addr);
                char ip_str[16];
                microlink_ip_to_str(ml->stun_fallback_ip, ip_str);
                ESP_LOGI(TAG, "STUN fallback resolved: %s -> %s",
                         ML_STUN_FALLBACK_HOST, ip_str);
            } else if (res->ai_family == AF_INET6) {
                
                if (memcmp(ml->stun_primary_ip6, zero16, 16) == 0) {
                    struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)res->ai_addr;
                    memcpy(ml->stun_primary_ip6, &addr6->sin6_addr, 16);
                    ESP_LOGI(TAG, "STUN fallback resolved: %s -> IPv6",
                             ML_STUN_FALLBACK_HOST);
                }
            }
            ml_freeaddrinfo(res);
        } else {
            ESP_LOGW(TAG, "DNS resolve failed for %s", ML_STUN_FALLBACK_HOST);
            if (res) ml_freeaddrinfo(res);
        }
    }

    bool have_ipv4 = (ml->stun_primary_ip || ml->stun_fallback_ip);
    bool have_ipv6 = (memcmp(ml->stun_primary_ip6, zero16, 16) != 0);
    return (have_ipv4 || have_ipv6) ? ESP_OK : ESP_FAIL;
}





esp_err_t ml_stun_send_probe(microlink_t *ml, const char *server, uint16_t port) {
    
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (ml_getaddrinfo(server, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolve failed for %s", server);
        return ESP_FAIL;
    }

    


    int sock;
    uint8_t *txid;
    if (res->ai_family == AF_INET6) {
        if (ensure_stun_socket6(ml) != ESP_OK) {
            ml_freeaddrinfo(res);
            return ESP_FAIL;
        }
        sock = ml->stun_sock6;
        txid = txid_v6;
    } else {
        
        if (ml->disco_sock4 >= 0) {
            sock = ml->disco_sock4;
        } else {
            if (ensure_stun_socket(ml) != ESP_OK) {
                ml_freeaddrinfo(res);
                return ESP_FAIL;
            }
            sock = ml->stun_sock;
        }
        txid = txid_v4;
    }

    
    uint8_t request[STUN_REQUEST_SIZE];
    size_t req_len = build_stun_request(request, txid);
    if (res->ai_family == AF_INET6)
        txid_v6_valid = true;
    else
        txid_v4_valid = true;

    
    int n = ml_sendto(sock, request, req_len, 0,
                   res->ai_addr, res->ai_addrlen);
    ml_freeaddrinfo(res);

    if (n < 0) {
        ESP_LOGE(TAG, "STUN send failed: %d", errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "STUN probe sent to %s:%u (%d bytes)", server, port, (int)req_len);
    return ESP_OK;
}





esp_err_t ml_stun_send_probe_to(microlink_t *ml, uint32_t server_ip, uint16_t port) {
    if (server_ip == 0) return ESP_ERR_INVALID_ARG;

    



    int sock = ml->disco_sock4;
    if (sock < 0) {
        
        if (ensure_stun_socket(ml) != ESP_OK) return ESP_FAIL;
        sock = ml->stun_sock;
    }

    
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(server_ip),
    };

    
    uint8_t request[STUN_REQUEST_SIZE];
    size_t req_len = build_stun_request(request, txid_v4);
    txid_v4_valid = true;

    
    int n = ml_sendto(sock, request, req_len, 0,
                   (struct sockaddr *)&dest, sizeof(dest));
    if (n < 0) {
        ESP_LOGE(TAG, "STUN send failed: %d", errno);
        return ESP_FAIL;
    }

    char ip_str[16];
    microlink_ip_to_str(server_ip, ip_str);
    ESP_LOGI(TAG, "STUN probe sent to %s:%u (%d bytes)", ip_str, port, (int)req_len);
    return ESP_OK;
}





bool ml_stun_parse_response(const uint8_t *data, size_t len,
                              uint32_t *out_ip, uint16_t *out_port) {
    if (len < STUN_HEADER_SIZE) {
        ESP_LOGW(TAG, "STUN response too short: %d", (int)len);
        return false;
    }

    
    uint16_t msg_type = (data[0] << 8) | data[1];
    if (msg_type != STUN_BINDING_RESPONSE) {
        ESP_LOGW(TAG, "Not a STUN binding response: 0x%04x", msg_type);
        return false;
    }

    
    uint32_t cookie = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    if (cookie != STUN_MAGIC_COOKIE) {
        ESP_LOGW(TAG, "Invalid STUN magic cookie");
        return false;
    }

    
    if (txid_v4_valid && memcmp(data + 8, txid_v4, 12) != 0) {
        
        ESP_LOGD(TAG, "STUN IPv4 txid mismatch (may be IPv6 response)");
        return false;
    }

    
    uint16_t msg_len = (data[2] << 8) | data[3];
    if (STUN_HEADER_SIZE + msg_len > len) {
        ESP_LOGW(TAG, "STUN message length exceeds packet");
        return false;
    }

    
    bool found_ipv6_only = false;
    size_t offset = STUN_HEADER_SIZE;
    while (offset + 4 <= STUN_HEADER_SIZE + msg_len) {
        uint16_t attr_type = (data[offset] << 8) | data[offset + 1];
        uint16_t attr_len = (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;

        if (offset + attr_len > len) break;

        if (attr_type == STUN_ATTR_XOR_MAPPED_ADDRESS) {
            if (attr_len < 8) break;

            
            uint8_t family = data[offset + 1];
            if (family == 0x01) {
                
                uint16_t xport = (data[offset + 2] << 8) | data[offset + 3];
                *out_port = xport ^ (STUN_MAGIC_COOKIE >> 16);

                
                uint32_t xaddr = (data[offset + 4] << 24) |
                                 (data[offset + 5] << 16) |
                                 (data[offset + 6] << 8) |
                                  data[offset + 7];
                *out_ip = xaddr ^ STUN_MAGIC_COOKIE;

                char ip_str[16];
                microlink_ip_to_str(*out_ip, ip_str);
                ESP_LOGI(TAG, "STUN mapped: %s:%u", ip_str, *out_port);
                txid_v4_valid = false;
                return true;
            }
            if (family == 0x02) {
                found_ipv6_only = true;  
            }
            offset += (attr_len + 3) & ~3;
            continue;
        }

        if (attr_type == STUN_ATTR_MAPPED_ADDRESS) {
            
            if (attr_len < 8) break;
            uint8_t family = data[offset + 1];
            if (family != 0x01) {
                offset += (attr_len + 3) & ~3;
                continue;
            }
            *out_port = (data[offset + 2] << 8) | data[offset + 3];
            *out_ip = (data[offset + 4] << 24) |
                      (data[offset + 5] << 16) |
                      (data[offset + 6] << 8) |
                       data[offset + 7];

            char ip_str[16];
            microlink_ip_to_str(*out_ip, ip_str);
            ESP_LOGI(TAG, "STUN mapped (legacy): %s:%u", ip_str, *out_port);
            txid_v4_valid = false;
            return true;
        }

        
        offset += (attr_len + 3) & ~3;
    }

    if (!found_ipv6_only) {
        ESP_LOGW(TAG, "STUN: no mapped address attribute found");
    }
    
    return false;
}





static esp_err_t ensure_stun_socket6(microlink_t *ml) {
    if (ml->stun_sock6 >= 0) return ESP_OK;

    ml->stun_sock6 = ml_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (ml->stun_sock6 < 0) {
        ESP_LOGD(TAG, "Failed to create IPv6 STUN socket: %d (expected on IPv4-only)", errno);
        return ESP_FAIL;
    }

    
    int flags = ml_fcntl(ml->stun_sock6, F_GETFL, 0);
    ml_fcntl(ml->stun_sock6, F_SETFL, flags | O_NONBLOCK);

    
    struct sockaddr_in6 bind_addr = {
        .sin6_family = AF_INET6,
        .sin6_port = 0,
        .sin6_addr = IN6ADDR_ANY_INIT,
    };
    ml_bind(ml->stun_sock6, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    return ESP_OK;
}

esp_err_t ml_stun_send_probe_ipv6(microlink_t *ml, const uint8_t *server_ip6, uint16_t port) {
    
    static const uint8_t zero16[16] = {0};
    if (memcmp(server_ip6, zero16, 16) == 0) return ESP_ERR_INVALID_ARG;

    if (ensure_stun_socket6(ml) != ESP_OK) return ESP_FAIL;

    struct sockaddr_in6 dest = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(port),
    };
    memcpy(&dest.sin6_addr, server_ip6, 16);

    
    uint8_t request[STUN_REQUEST_SIZE];
    size_t req_len = build_stun_request(request, txid_v6);
    txid_v6_valid = true;

    int n = ml_sendto(ml->stun_sock6, request, req_len, 0,
                   (struct sockaddr *)&dest, sizeof(dest));
    if (n < 0) {
        
        if (errno == 118 || errno == 51 || errno == ENETUNREACH) {
            ESP_LOGD(TAG, "STUN IPv6 send: no IPv6 route (errno %d)", errno);
        } else {
            ESP_LOGE(TAG, "STUN IPv6 send failed: %d", errno);
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "STUN IPv6 probe sent (%d bytes)", (int)req_len);
    return ESP_OK;
}

bool ml_stun_parse_response_ipv6(const uint8_t *data, size_t len,
                                  uint8_t *out_ip6, uint16_t *out_port) {
    if (len < STUN_HEADER_SIZE) return false;

    
    uint16_t msg_type = (data[0] << 8) | data[1];
    if (msg_type != STUN_BINDING_RESPONSE) return false;

    
    uint32_t cookie = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    if (cookie != STUN_MAGIC_COOKIE) return false;

    

    bool matched_v6 = (txid_v6_valid && memcmp(data + 8, txid_v6, 12) == 0);
    bool matched_v4 = (!matched_v6 && txid_v4_valid && memcmp(data + 8, txid_v4, 12) == 0);
    if (!matched_v6 && !matched_v4) return false;

    
    const uint8_t *matched_txid = matched_v6 ? txid_v6 : txid_v4;

    uint16_t msg_len = (data[2] << 8) | data[3];
    if (STUN_HEADER_SIZE + msg_len > len) return false;

    size_t offset = STUN_HEADER_SIZE;
    while (offset + 4 <= STUN_HEADER_SIZE + msg_len) {
        uint16_t attr_type = (data[offset] << 8) | data[offset + 1];
        uint16_t attr_len = (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;

        if (offset + attr_len > len) break;

        if (attr_type == STUN_ATTR_XOR_MAPPED_ADDRESS) {
            uint8_t family = data[offset + 1];

            if (family == 0x02 && attr_len >= 20) {
                
                uint16_t xport = (data[offset + 2] << 8) | data[offset + 3];
                *out_port = xport ^ (STUN_MAGIC_COOKIE >> 16);

                

                uint8_t xor_bytes[16];
                xor_bytes[0] = 0x21; xor_bytes[1] = 0x12;
                xor_bytes[2] = 0xA4; xor_bytes[3] = 0x42;
                memcpy(xor_bytes + 4, matched_txid, 12);

                for (int i = 0; i < 16; i++) {
                    out_ip6[i] = data[offset + 4 + i] ^ xor_bytes[i];
                }

                ESP_LOGI(TAG, "STUN IPv6 mapped: port %u", *out_port);
                if (matched_v6) txid_v6_valid = false;
                else txid_v4_valid = false;
                return true;
            }

            if (family == 0x01 && attr_len >= 8) {
                
                offset += (attr_len + 3) & ~3;
                continue;
            }
        }

        offset += (attr_len + 3) & ~3;
    }

    return false;
}
