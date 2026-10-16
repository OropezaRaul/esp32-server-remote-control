














#pragma once

#include "esp_err.h"
#include "microlink.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif





typedef enum {
    ML_NET_WIFI = 0,
    ML_NET_CELLULAR,
} ml_net_type_t;

typedef enum {
    ML_NET_SW_IDLE = 0,             
    ML_NET_SW_WIFI_CONNECTING,      
    ML_NET_SW_WIFI_VPN_UP,          
    ML_NET_SW_SWITCHING_TO_CELL,    
    ML_NET_SW_CELL_CONNECTING,      
    ML_NET_SW_CELL_VPN_UP,          
    ML_NET_SW_SWITCHING_TO_WIFI,    
    ML_NET_SW_ERROR,                
} ml_net_switch_state_t;

typedef struct {
    
    const char *wifi_ssid;
    const char *wifi_password;

    
    const char *sim_pin;
    const char *apn;

    
    const char *tailscale_auth_key;
    const char *device_name;

    
    uint32_t wifi_timeout_ms;           
    uint32_t health_check_interval_ms;  
    uint32_t health_fail_count;         
    uint32_t failback_check_ms;         

    
    void (*on_connected)(ml_net_type_t type, void *user_data);
    void (*on_disconnected)(void *user_data);
    void (*on_switching)(ml_net_type_t from, ml_net_type_t to, void *user_data);
    void *user_data;
} ml_net_switch_config_t;












esp_err_t ml_net_switch_init(const ml_net_switch_config_t *config);




void ml_net_switch_deinit(void);







esp_err_t ml_net_switch_start(void);






esp_err_t ml_net_switch_stop(void);






ml_net_type_t ml_net_switch_get_active(void);




ml_net_switch_state_t ml_net_switch_get_state(void);





microlink_t *ml_net_switch_get_handle(void);





microlink_udp_socket_t *ml_net_switch_get_udp_socket(void);




const char *ml_net_switch_state_str(ml_net_switch_state_t state);

#ifdef __cplusplus
}
#endif
