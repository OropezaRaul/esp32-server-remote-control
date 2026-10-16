















#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef CONFIG_ML_ENABLE_CONFIG_HTTPD

#ifdef __cplusplus
extern "C" {
#endif


struct microlink_s;





#define ML_CONFIG_MAX_ALLOWED_PEERS  CONFIG_ML_CONFIG_MAX_ALLOWED_PEERS







#define ML_CONFIG_SETTINGS_VERSION  3

typedef struct __attribute__((packed)) {
    uint8_t  version;               

    
    char     wifi_ssid[33];         
    char     wifi_pass[65];         
    char     auth_key[96];          
    char     device_prefix[32];     
    char     cellular_apn[32];
    char     cellular_sim_pin[16];
    uint8_t  flags;                 

    
    uint8_t  max_peers;             
    uint16_t disco_heartbeat_ms;    
    uint32_t priority_peer_ip;      
    char     ctrl_host[64];         
    uint8_t  debug_flags;           


    
    char     device_name_full[48];  

    char     ppp_user[32];          
    char     ppp_pass[32];          
} ml_config_settings_t;


typedef struct __attribute__((packed)) {
    uint32_t vpn_ip;                
    char     label[24];             
} ml_config_peer_entry_t;


typedef struct __attribute__((packed)) {
    uint16_t count;                 
    ml_config_peer_entry_t entries[ML_CONFIG_MAX_ALLOWED_PEERS];
} ml_config_peer_list_t;


#define ML_CONFIG_MAX_WIFI_ENTRIES  16

typedef struct __attribute__((packed)) {
    char ssid[33];                  
    char pass[65];                  
} ml_config_wifi_entry_t;           

typedef struct __attribute__((packed)) {
    uint8_t count;                  
    uint8_t active_idx;             
    ml_config_wifi_entry_t entries[ML_CONFIG_MAX_WIFI_ENTRIES];
} ml_config_wifi_list_t;            


typedef struct ml_config_ctx ml_config_ctx_t;












ml_config_ctx_t *ml_config_httpd_init(void);










esp_err_t ml_config_httpd_start(ml_config_ctx_t *ctx, struct microlink_s *ml);




void ml_config_httpd_stop(ml_config_ctx_t *ctx);




void ml_config_httpd_deinit(ml_config_ctx_t *ctx);














bool ml_config_peer_is_allowed(const ml_config_ctx_t *ctx, uint32_t vpn_ip);





const char *ml_config_get_wifi_ssid(const ml_config_ctx_t *ctx);
const char *ml_config_get_wifi_pass(const ml_config_ctx_t *ctx);
const char *ml_config_get_auth_key(const ml_config_ctx_t *ctx);
const char *ml_config_get_device_prefix(const ml_config_ctx_t *ctx);


uint8_t     ml_config_get_max_peers(const ml_config_ctx_t *ctx);
uint16_t    ml_config_get_disco_heartbeat_ms(const ml_config_ctx_t *ctx);
uint32_t    ml_config_get_priority_peer_ip(const ml_config_ctx_t *ctx);
const char *ml_config_get_ctrl_host(const ml_config_ctx_t *ctx);
uint8_t     ml_config_get_debug_flags(const ml_config_ctx_t *ctx);


const char *ml_config_get_device_name_full(const ml_config_ctx_t *ctx);










bool ml_config_get_nvs_wifi(char *ssid, size_t ssid_len,
                             char *pass, size_t pass_len);








bool ml_config_get_nvs_ppp(char *user, size_t user_len,
                            char *pass, size_t pass_len);







bool ml_config_get_nvs_apn(char *apn, size_t apn_len);











bool ml_config_get_wifi_list(ml_config_wifi_list_t *list);

#ifdef __cplusplus
}
#endif

#else 


typedef void ml_config_ctx_t;


#ifndef ML_CONFIG_MAX_WIFI_ENTRIES
#define ML_CONFIG_MAX_WIFI_ENTRIES  16
typedef struct __attribute__((packed)) {
    char ssid[33];
    char pass[65];
} ml_config_wifi_entry_t;
typedef struct __attribute__((packed)) {
    uint8_t count;
    uint8_t active_idx;
    ml_config_wifi_entry_t entries[ML_CONFIG_MAX_WIFI_ENTRIES];
} ml_config_wifi_list_t;
#endif

static inline ml_config_ctx_t *ml_config_httpd_init(void) { return NULL; }
static inline int ml_config_httpd_start(ml_config_ctx_t *ctx, void *ml) { (void)ctx; (void)ml; return 0; }
static inline void ml_config_httpd_stop(ml_config_ctx_t *ctx) { (void)ctx; }
static inline void ml_config_httpd_deinit(ml_config_ctx_t *ctx) { (void)ctx; }
static inline bool ml_config_peer_is_allowed(const ml_config_ctx_t *ctx, uint32_t ip) { (void)ctx; (void)ip; return true; }
static inline bool ml_config_get_nvs_wifi(char *s, size_t sl, char *p, size_t pl) { (void)s;(void)sl;(void)p;(void)pl; return false; }
static inline bool ml_config_get_nvs_ppp(char *u, size_t ul, char *p, size_t pl) { (void)u;(void)ul;(void)p;(void)pl; return false; }
static inline bool ml_config_get_nvs_apn(char *a, size_t al) { (void)a;(void)al; return false; }
static inline bool ml_config_get_wifi_list(ml_config_wifi_list_t *l) { (void)l; return false; }


static inline const char *ml_config_get_wifi_ssid(const ml_config_ctx_t *c) { (void)c; return NULL; }
static inline const char *ml_config_get_wifi_pass(const ml_config_ctx_t *c) { (void)c; return NULL; }
static inline const char *ml_config_get_auth_key(const ml_config_ctx_t *c) { (void)c; return NULL; }
static inline const char *ml_config_get_device_prefix(const ml_config_ctx_t *c) { (void)c; return NULL; }
static inline const char *ml_config_get_device_name_full(const ml_config_ctx_t *c) { (void)c; return NULL; }
static inline uint8_t ml_config_get_max_peers(const ml_config_ctx_t *c) { (void)c; return 0; }
static inline uint16_t ml_config_get_disco_heartbeat_ms(const ml_config_ctx_t *c) { (void)c; return 0; }
static inline uint32_t ml_config_get_priority_peer_ip(const ml_config_ctx_t *c) { (void)c; return 0; }
static inline const char *ml_config_get_ctrl_host(const ml_config_ctx_t *c) { (void)c; return NULL; }
static inline uint8_t ml_config_get_debug_flags(const ml_config_ctx_t *c) { (void)c; return 0; }

#endif 
