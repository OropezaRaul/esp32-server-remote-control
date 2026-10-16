














#pragma once

#include "microlink.h"
#include "lwip/ip_addr.h"
#include "ml_config_httpd.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "esp_heap_caps.h"

#ifdef CONFIG_ML_ZERO_COPY_WG
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif
















#ifndef MICROLINK_IPN_VERSION
#define MICROLINK_IPN_VERSION    "1.78.1-microlink-funnel-native"
#endif

#ifndef MICROLINK_APP_NAME
#define MICROLINK_APP_NAME       "esp32-remote-control"
#endif


#define ML_TASK_NET_IO_STACK    (8 * 1024)
#define ML_TASK_NET_IO_PRIO     7
#define ML_TASK_NET_IO_CORE     0

#define ML_TASK_DERP_TX_STACK   (14 * 1024)
#define ML_TASK_DERP_TX_PRIO    5
#define ML_TASK_DERP_TX_CORE    0

#define ML_TASK_COORD_STACK     (12 * 1024)
#define ML_TASK_COORD_PRIO      5
#define ML_TASK_COORD_CORE      1

#define ML_TASK_WG_MGR_STACK    (8 * 1024)
#define ML_TASK_WG_MGR_PRIO     7
#define ML_TASK_WG_MGR_CORE     1


#define ML_DERP_TX_QUEUE_DEPTH  16
#define ML_DISCO_RX_QUEUE_DEPTH 8
#define ML_WG_RX_QUEUE_DEPTH    8
#define ML_STUN_RX_QUEUE_DEPTH  4
#define ML_COORD_CMD_QUEUE_DEPTH 4
#define ML_PEER_UPDATE_QUEUE_DEPTH 400


#define ML_MAX_PEERS            CONFIG_ML_MAX_PEERS
#define ML_MAX_ENDPOINTS        8
#define ML_MAX_PACKET_SIZE      1500
#define ML_DERP_MAX_FRAME       (ML_MAX_PACKET_SIZE + 64)


#define ML_DERP_REGION          9       
#define ML_DERP_HOST            "derp9e.tailscale.com"
#define ML_DERP_PORT            443


#define ML_CTRL_HOST            "controlplane.tailscale.com"
#define ML_CTRL_PORT            443
#define ML_CTRL_PROTOCOL_VER    131


#define ML_DISCO_PING_INTERVAL_MS       5000
#define ML_DISCO_HEARTBEAT_MS           3000
#define ML_DISCO_TRUST_DURATION_MS      15000
#define ML_DISCO_PING_TIMEOUT_MS        5000
#define ML_DISCO_UPGRADE_INTERVAL_MS    15000
#define ML_DISCO_SESSION_ACTIVE_MS      45000


#define ML_STUN_PRIMARY_HOST    "derp9.tailscale.com"
#define ML_STUN_PRIMARY_PORT    3478
#define ML_STUN_FALLBACK_HOST   "stun.l.google.com"
#define ML_STUN_FALLBACK_PORT   19302
#define ML_STUN_MAX_RETRIES     3
#define ML_STUN_RETRY_INTERVAL_MS   2000


#define ML_STUN_RETRANSMIT_MS           100
#define ML_STUN_TOTAL_TIMEOUT_MS        5000
#define ML_STUN_RESTUN_INTERVAL_MS      23000


#define ML_CTRL_WATCHDOG_MS             120000
#define ML_CTRL_BACKOFF_MAX_MS          30000
#define ML_CTRL_KEEPALIVE_MS            60000


#define ML_H2_BUFFER_SIZE       (CONFIG_ML_H2_BUFFER_SIZE_KB * 1024)
#define ML_JSON_BUFFER_SIZE     (CONFIG_ML_JSON_BUFFER_SIZE_KB * 1024)


#define ML_NOISE_KEY_LEN        32
#define ML_NOISE_MAC_LEN        16
#define ML_NOISE_NONCE_LEN      12
#define ML_NOISE_HASH_LEN       32









#ifdef CONFIG_ML_ZERO_COPY_WG

#define ML_ZC_DISCO_RING_SIZE   16      
#define ML_ZC_DISCO_MAX_PKT     256     
#define ML_ZC_TX_POOL_SIZE      24      


typedef struct {
    uint8_t data[ML_ZC_DISCO_MAX_PKT];
    uint16_t len;
    uint32_t src_ip_nbo;    
    uint16_t src_port;      
} ml_zc_disco_entry_t;


typedef struct {
    struct udp_pcb *pcb;
    uint8_t data[ML_MAX_PACKET_SIZE];
    uint16_t len;
    ip_addr_t dest;
    uint16_t port;
} ml_zc_tx_ctx_t;


typedef struct {
    struct udp_pcb *pcb;            
    uint16_t local_port;            

    
    ml_zc_disco_entry_t rx_ring[ML_ZC_DISCO_RING_SIZE];
    volatile uint8_t rx_head;       
    volatile uint8_t rx_tail;       

    
    ml_zc_tx_ctx_t tx_pool[ML_ZC_TX_POOL_SIZE];
    volatile uint8_t tx_head;       
    volatile uint8_t tx_tail;       
} ml_zerocopy_t;

#endif 





#define ML_EVT_WIFI_CONNECTED       BIT0
#define ML_EVT_COORD_REGISTERED     BIT1
#define ML_EVT_DERP_CONNECTED       BIT2
#define ML_EVT_WG_READY             BIT3
#define ML_EVT_PEERS_AVAILABLE      BIT4
#define ML_EVT_STUN_COMPLETE        BIT5
#define ML_EVT_SHUTDOWN_REQUEST     BIT6
#define ML_EVT_DERP_RECONNECT       BIT7
#define ML_EVT_DERP_CONNECT_REQ     BIT8






typedef struct {
    uint8_t dest_pubkey[32];    
    uint8_t *data;              
    size_t len;                 
    uint8_t frame_type;         
} ml_derp_tx_item_t;


typedef struct {
    uint8_t *data;              
    size_t len;                 
    uint32_t src_ip;            
    uint16_t src_port;          
    uint8_t src_pubkey[32];     
    bool via_derp;              
} ml_rx_packet_t;


typedef enum {
    ML_CMD_CONNECT,             
    ML_CMD_DISCONNECT,          
    ML_CMD_UPDATE_ENDPOINTS,    
    ML_CMD_FORCE_RECONNECT,     
} ml_coord_cmd_t;


typedef struct {
    enum {
        ML_PEER_ADD,
        ML_PEER_REMOVE,
        ML_PEER_UPDATE_ENDPOINT,
    } action;
    uint32_t vpn_ip;
    ip6_addr_t vpn_ipv6;
    bool is_ipv6;
    bool has_ipv6;
    uint8_t public_key[32];
    uint8_t disco_key[32];
    char hostname[64];
    uint16_t derp_region;
    
    struct {
        uint32_t ip;
        uint16_t port;
        bool is_ipv6;
    } endpoints[ML_MAX_ENDPOINTS];
    int endpoint_count;
} ml_peer_update_t;





typedef struct {
    
    uint32_t vpn_ip;
    ip6_addr_t vpn_ipv6;
    bool is_ipv6;
    bool has_ipv6;
    uint8_t public_key[32];
    uint8_t disco_key[32];
    char hostname[64];
    bool active;

    
    struct {
        uint32_t ip;
        uint16_t port;
        bool is_ipv6;
    } endpoints[ML_MAX_ENDPOINTS];
    int endpoint_count;
    uint16_t derp_region;

    
    uint64_t last_ping_sent_ms;     
    uint64_t last_pong_recv_ms;     
    uint64_t trust_until_ms;        
    uint64_t last_send_ms;          
    uint64_t last_upgrade_ms;       

    
    uint32_t best_ip;
    uint16_t best_port;
    bool has_direct_path;

    
    int wg_peer_index;

    
    bool tried_initial_handshake;
} ml_peer_t;





#define ML_MAX_DERP_REGIONS     32
#define ML_MAX_DERP_NODES       4

typedef struct {
    char hostname[64];
    char ipv4[16];
    char ipv6[46];
    uint16_t stun_port;     
    uint16_t derp_port;     
    bool stun_only;         
} ml_derp_node_t;

typedef struct {
    uint16_t region_id;
    char code[8];           
    ml_derp_node_t nodes[ML_MAX_DERP_NODES];
    uint8_t node_count;
    bool avoid;             
} ml_derp_region_t;





typedef struct {
    uint8_t h[ML_NOISE_HASH_LEN];
    uint8_t ck[ML_NOISE_HASH_LEN];
    uint8_t local_static_private[32];
    uint8_t local_static_public[32];
    uint8_t local_ephemeral_private[32];
    uint8_t local_ephemeral_public[32];
    uint8_t remote_static_public[32];
    uint8_t tx_key[ML_NOISE_KEY_LEN];
    uint8_t rx_key[ML_NOISE_KEY_LEN];
    uint64_t tx_nonce;
    uint64_t rx_nonce;
    bool handshake_complete;
} ml_noise_state_t;





typedef struct {
    int sockfd;                     
    mbedtls_ssl_context ssl;        
    mbedtls_ssl_config ssl_conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool connected;
    uint64_t last_recv_ms;          
} ml_derp_conn_t;





struct microlink_s {
    
    microlink_config_t config;

    
    volatile microlink_state_t state;
    volatile uint32_t vpn_ip;
    ip6_addr_t vpn_ipv6;

    


    char self_fqdn[144];

    
    EventGroupHandle_t events;

    
    TaskHandle_t net_io_task;
    TaskHandle_t derp_tx_task;
    TaskHandle_t coord_task;
    TaskHandle_t wg_mgr_task;

    
    QueueHandle_t derp_tx_queue;        
    QueueHandle_t disco_rx_queue;       
    QueueHandle_t wg_rx_queue;          
    QueueHandle_t stun_rx_queue;        
    QueueHandle_t coord_cmd_queue;      
    QueueHandle_t peer_update_queue;    

    
    uint8_t machine_private_key[32];    
    uint8_t machine_public_key[32];
    uint8_t wg_private_key[32];         
    uint8_t wg_public_key[32];
    uint8_t disco_private_key[32];      
    uint8_t disco_public_key[32];

    
    
    ml_derp_conn_t derp;

    
    int disco_sock4;                    
    int disco_sock6;                    
    int stun_sock;                      
    uint16_t disco_local_port;          

    
    int coord_sock;
    





    volatile uint32_t last_ctrl_rx_ms;
    uint32_t h2_next_stream_id;         

    
    void *wg_netif;

    
    ml_peer_t peers[ML_MAX_PEERS];
    int peer_count;

    
    uint32_t stun_public_ip;
    uint16_t stun_public_port;

    
    uint32_t stun_primary_ip;       
    uint32_t stun_fallback_ip;      
    uint8_t stun_retry_count;       
    bool stun_using_fallback;       
    uint64_t stun_last_probe_ms;    

    
    int stun_sock6;                 
    uint8_t stun_primary_ip6[16];   
    uint8_t stun_public_ip6[16];    
    uint16_t stun_public_port6;     
    bool stun_has_ipv6;             

    
    uint16_t stun_secondary_port;   
    bool stun_nat_checked;          
    bool nat_mapping_varies;        

    
    ml_derp_region_t derp_regions[ML_MAX_DERP_REGIONS];
    uint8_t derp_region_count;
    uint16_t derp_home_region;      

    
    int64_t key_expiry_epoch;       
    bool key_expired;               

    
    uint32_t t_disco_heartbeat_ms;
    uint32_t t_stun_interval_ms;
    uint32_t t_ctrl_watchdog_ms;

    
    microlink_state_cb_t state_cb;
    void *state_cb_data;
    microlink_peer_cb_t peer_cb;
    void *peer_cb_data;
    microlink_data_cb_t data_cb;
    void *data_cb_data;

    
    ml_config_ctx_t *config_httpd;

    

    char nvs_auth_key[96];
    char nvs_device_name[48];

    

    char ctrl_host[64];

    
    uint8_t debug_flags;  

#ifdef CONFIG_ML_ZERO_COPY_WG
    
    ml_zerocopy_t zc;
#endif
};






void ml_net_io_task(void *arg);


void ml_derp_tx_task(void *arg);
esp_err_t ml_derp_connect(microlink_t *ml);
void ml_derp_disconnect(microlink_t *ml);
esp_err_t ml_derp_queue_send(microlink_t *ml, const uint8_t *dest_key,
                              const uint8_t *data, size_t len);


void ml_coord_task(void *arg);


void ml_wg_mgr_task(void *arg);
void ml_wg_mgr_send_cmm(microlink_t *ml, uint32_t peer_vpn_ip);
esp_err_t ml_wg_mgr_trigger_handshake(microlink_t *ml, uint32_t dest_vpn_ip);
bool ml_wg_mgr_peer_is_up(microlink_t *ml, uint32_t vpn_ip);
void ml_wg_mgr_update_transport(microlink_t *ml);


esp_err_t ml_stun_resolve_servers(microlink_t *ml);
esp_err_t ml_stun_send_probe(microlink_t *ml, const char *server, uint16_t port);
esp_err_t ml_stun_send_probe_to(microlink_t *ml, uint32_t server_ip, uint16_t port);
esp_err_t ml_stun_send_probe_ipv6(microlink_t *ml, const uint8_t *server_ip6, uint16_t port);
bool ml_stun_parse_response(const uint8_t *data, size_t len,
                             uint32_t *out_ip, uint16_t *out_port);
bool ml_stun_parse_response_ipv6(const uint8_t *data, size_t len,
                                  uint8_t *out_ip6, uint16_t *out_port);


void ml_noise_init(ml_noise_state_t *state,
                    const uint8_t *local_private, const uint8_t *local_public,
                    const uint8_t *remote_public);
esp_err_t ml_noise_write_msg1(ml_noise_state_t *state, uint8_t *out, size_t *out_len);
esp_err_t ml_noise_read_msg2(ml_noise_state_t *state, const uint8_t *msg, size_t len);
esp_err_t ml_noise_encrypt(const uint8_t *key, uint64_t nonce,
                            const uint8_t *ad, size_t ad_len,
                            const uint8_t *plaintext, size_t pt_len,
                            uint8_t *ciphertext);
esp_err_t ml_noise_decrypt(const uint8_t *key, uint64_t nonce,
                            const uint8_t *ad, size_t ad_len,
                            const uint8_t *ciphertext, size_t ct_len,
                            uint8_t *plaintext);


int ml_h2_build_headers_frame(uint8_t *out, size_t out_size,
                               const char *method, const char *path,
                               const char *authority, const char *content_type,
                               uint32_t stream_id, bool end_stream);
int ml_h2_build_data_frame(uint8_t *out, size_t out_size,
                            const uint8_t *data, size_t data_len,
                            uint32_t stream_id, bool end_stream);
int ml_h2_build_preface(uint8_t *out, size_t out_size);
int ml_h2_build_settings_ack(uint8_t *out, size_t out_size);
int ml_h2_build_window_update(uint8_t *out, size_t out_size,
                               uint32_t stream_id, uint32_t increment);


esp_err_t ml_peer_nvs_init(void);
void ml_peer_nvs_deinit(void);
esp_err_t ml_peer_nvs_save(const ml_peer_t *peer);
int ml_peer_nvs_load_all(ml_peer_t *peers, int max_peers);
esp_err_t ml_peer_nvs_clear(void);

#ifdef CONFIG_ML_ZERO_COPY_WG

esp_err_t ml_zerocopy_init(microlink_t *ml);
void ml_zerocopy_deinit(microlink_t *ml);
esp_err_t ml_zerocopy_send(microlink_t *ml, const uint8_t *data, size_t len,
                            uint32_t dest_ip, uint16_t dest_port);
#endif


uint64_t ml_get_time_ms(void);









#include "ml_at_socket.h"

#ifdef CONFIG_ML_ENABLE_CELLULAR
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

static inline int ml_socket(int domain, int type, int protocol) {
    if (ml_at_socket_is_ready() && (domain == AF_INET || domain == AF_INET6))
        return ml_at_socket(domain, type, protocol);
    return socket(domain, type, protocol);
}

static inline int ml_connect(int fd, const struct sockaddr *addr, socklen_t len) {
    if (ml_at_socket_is_at_fd(fd))
        return ml_at_connect(fd, addr, len);
    return connect(fd, addr, len);
}

static inline ssize_t ml_send(int fd, const void *buf, size_t len, int flags) {
    if (ml_at_socket_is_at_fd(fd))
        return ml_at_send(fd, buf, len, flags);
    return send(fd, buf, len, flags);
}

static inline ssize_t ml_recv(int fd, void *buf, size_t len, int flags) {
    if (ml_at_socket_is_at_fd(fd))
        return ml_at_recv(fd, buf, len, flags);
    return recv(fd, buf, len, flags);
}

static inline ssize_t ml_sendto(int fd, const void *buf, size_t len, int flags,
                                 const struct sockaddr *addr, socklen_t addrlen) {
    if (ml_at_socket_is_at_fd(fd))
        return ml_at_sendto(fd, buf, len, flags, addr, addrlen);
    return sendto(fd, buf, len, flags, addr, addrlen);
}

static inline ssize_t ml_recvfrom(int fd, void *buf, size_t len, int flags,
                                    struct sockaddr *addr, socklen_t *addrlen) {
    if (ml_at_socket_is_at_fd(fd))
        return ml_at_recvfrom(fd, buf, len, flags, addr, addrlen);
    return recvfrom(fd, buf, len, flags, addr, addrlen);
}

static inline int ml_close_sock(int fd) {
    if (ml_at_socket_is_at_fd(fd))
        return ml_at_close(fd);
    return close(fd);
}

static inline int ml_bind(int fd, const struct sockaddr *addr, socklen_t len) {
    if (ml_at_socket_is_at_fd(fd))
        return ml_at_bind(fd, addr, len);
    return bind(fd, addr, len);
}

static inline int ml_setsockopt(int fd, int level, int optname,
                                  const void *optval, socklen_t optlen) {
    if (ml_at_socket_is_at_fd(fd))
        return ml_at_setsockopt(fd, level, optname, optval, optlen);
    return setsockopt(fd, level, optname, optval, optlen);
}

static inline int ml_fcntl(int fd, int cmd, int arg) {
    if (ml_at_socket_is_at_fd(fd))
        return ml_at_fcntl(fd, cmd, arg);
    return fcntl(fd, cmd, arg);
}

static inline int ml_select_fds(int nfds, fd_set *rd, fd_set *wr,
                                  fd_set *ex, struct timeval *tv) {
    
    if (ml_at_socket_is_ready() && nfds > ML_AT_SOCK_FD_BASE)
        return ml_at_select(nfds, rd, wr, ex, tv);
    return select(nfds, rd, wr, ex, tv);
}

static inline int ml_getaddrinfo(const char *host, const char *svc,
                                   const struct addrinfo *hints,
                                   struct addrinfo **res) {
    if (ml_at_socket_is_ready())
        return ml_at_getaddrinfo(host, svc, hints, res);
    return getaddrinfo(host, svc, hints, res);
}

static inline void ml_freeaddrinfo(struct addrinfo *res) {
    
    if (ml_at_socket_is_ready()) {
        ml_at_freeaddrinfo(res);
        return;
    }
    freeaddrinfo(res);
}


static inline ssize_t ml_write_sock(int fd, const void *buf, size_t len) {
    if (ml_at_socket_is_at_fd(fd))
        return ml_at_write(fd, buf, len);
    return write(fd, buf, len);
}

static inline ssize_t ml_read_sock(int fd, void *buf, size_t len) {
    if (ml_at_socket_is_at_fd(fd))
        return ml_at_read(fd, buf, len);
    return read(fd, buf, len);
}

#else

#define ml_socket       socket
#define ml_connect      connect
#define ml_send         send
#define ml_recv         recv
#define ml_sendto       sendto
#define ml_recvfrom     recvfrom
#define ml_close_sock   close
#define ml_bind         bind
#define ml_setsockopt   setsockopt
#define ml_fcntl        fcntl
#define ml_select_fds   select
#define ml_getaddrinfo  getaddrinfo
#define ml_freeaddrinfo freeaddrinfo
#define ml_write_sock   write
#define ml_read_sock    read
#endif 


static inline void *ml_psram_malloc(size_t size) {
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = malloc(size);
    return ptr;
}

static inline void *ml_psram_calloc(size_t n, size_t size) {
    void *ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = calloc(n, size);
    return ptr;
}

#ifdef __cplusplus
}
#endif
