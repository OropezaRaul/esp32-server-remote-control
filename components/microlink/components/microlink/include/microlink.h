











#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct microlink_s microlink_t;


typedef struct {
    const char *auth_key;       
    const char *device_name;    
    bool enable_derp;           
    bool enable_stun;           
    bool enable_disco;          
    uint8_t max_peers;          
    int8_t wifi_tx_power_dbm;   

    





    uint32_t priority_peer_ip;  

    
    uint32_t disco_heartbeat_ms;    
    uint32_t stun_interval_ms;      
    uint32_t ctrl_watchdog_ms;      

    






    bool enable_funnel;

    






    uint16_t peerapi_port;
} microlink_config_t;


typedef struct {
    uint32_t vpn_ip;
    char hostname[64];
    uint8_t public_key[32];
    bool online;
    bool direct_path;           
} microlink_peer_info_t;


typedef enum {
    ML_STATE_IDLE = 0,
    ML_STATE_WIFI_WAIT,
    ML_STATE_CONNECTING,
    ML_STATE_REGISTERING,
    ML_STATE_CONNECTED,
    ML_STATE_RECONNECTING,
    ML_STATE_ERROR,
} microlink_state_t;


typedef void (*microlink_state_cb_t)(microlink_t *ml, microlink_state_t state, void *user_data);
typedef void (*microlink_peer_cb_t)(microlink_t *ml, const microlink_peer_info_t *peer, void *user_data);
typedef void (*microlink_data_cb_t)(microlink_t *ml, uint32_t src_ip, const uint8_t *data,
                                     size_t len, void *user_data);










esp_err_t microlink_factory_reset(void);









microlink_t *microlink_init(const microlink_config_t *config);









esp_err_t microlink_start(microlink_t *ml);

















esp_err_t microlink_rebind(microlink_t *ml);








esp_err_t microlink_stop(microlink_t *ml);





void microlink_destroy(microlink_t *ml);




microlink_state_t microlink_get_state(const microlink_t *ml);











uint32_t microlink_get_ctrl_stale_ms(const microlink_t *ml);




bool microlink_is_connected(const microlink_t *ml);





uint32_t microlink_get_vpn_ip(const microlink_t *ml);




int microlink_get_peer_count(const microlink_t *ml);








esp_err_t microlink_get_peer_info(const microlink_t *ml, int index, microlink_peer_info_t *info);









esp_err_t microlink_send(microlink_t *ml, uint32_t dest_vpn_ip,
                          const uint8_t *data, size_t len);




void microlink_set_state_callback(microlink_t *ml, microlink_state_cb_t cb, void *user_data);
void microlink_set_peer_callback(microlink_t *ml, microlink_peer_cb_t cb, void *user_data);
void microlink_set_data_callback(microlink_t *ml, microlink_data_cb_t cb, void *user_data);




void microlink_ip_to_str(uint32_t ip, char *buf);





uint32_t microlink_parse_ip(const char *ip_str);





const char *microlink_default_device_name(void);








const char *microlink_imei_device_name(void);





const char *microlink_get_fqdn(const microlink_t *ml);













esp_err_t microlink_set_dns(const char *name, const char *value, uint32_t timeout_ms);



















uint32_t microlink_resolve(const microlink_t *ml, const char *hostname);









typedef struct microlink_udp_socket microlink_udp_socket_t;


typedef void (*microlink_udp_rx_cb_t)(microlink_udp_socket_t *sock,
                                       uint32_t src_ip, uint16_t src_port,
                                       const uint8_t *data, size_t len,
                                       void *user_data);









microlink_udp_socket_t *microlink_udp_create(microlink_t *ml, uint16_t local_port);




void microlink_udp_close(microlink_udp_socket_t *sock);










esp_err_t microlink_udp_send(microlink_udp_socket_t *sock, uint32_t dest_ip,
                              uint16_t dest_port, const void *data, size_t len);











esp_err_t microlink_udp_recv(microlink_udp_socket_t *sock, uint32_t *src_ip,
                              uint16_t *src_port, void *buffer, size_t *len,
                              uint32_t timeout_ms);







esp_err_t microlink_udp_set_rx_callback(microlink_udp_socket_t *sock,
                                         microlink_udp_rx_cb_t cb, void *user_data);




uint16_t microlink_udp_get_local_port(const microlink_udp_socket_t *sock);











typedef struct microlink_tcp_socket microlink_tcp_socket_t;












microlink_tcp_socket_t *microlink_tcp_connect(microlink_t *ml, uint32_t dest_ip,
                                                uint16_t dest_port,
                                                uint32_t timeout_ms);










esp_err_t microlink_tcp_send(microlink_tcp_socket_t *sock, const void *data, size_t len);









int microlink_tcp_recv(microlink_tcp_socket_t *sock, void *buffer, size_t len,
                        uint32_t timeout_ms);




bool microlink_tcp_is_connected(const microlink_tcp_socket_t *sock);




void microlink_tcp_close(microlink_tcp_socket_t *sock);

#ifdef __cplusplus
}
#endif
