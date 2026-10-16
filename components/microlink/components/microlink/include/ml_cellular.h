













#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif





typedef struct {
    int tx_pin;                 
    int rx_pin;                 
    int baud_rate;              
    const char *apn;            
    const char *sim_pin;        
    const char *ppp_user;       
    const char *ppp_pass;       
} ml_cellular_config_t;


#define ML_CELLULAR_DEFAULT_CONFIG() { \
    .tx_pin = 43,               \
    .rx_pin = 44,               \
    .baud_rate = 115200,        \
    .apn = NULL,                \
    .sim_pin = NULL,            \
    .ppp_user = NULL,           \
    .ppp_pass = NULL,           \
}





typedef enum {
    ML_CELL_STATE_OFF,          
    ML_CELL_STATE_INIT,         
    ML_CELL_STATE_AT_OK,        
    ML_CELL_STATE_SIM_READY,    
    ML_CELL_STATE_REGISTERED,   
    ML_CELL_STATE_PPP_CONNECTING, 
    ML_CELL_STATE_PPP_CONNECTED,  
    ML_CELL_STATE_DATA_CONNECTED, 
    ML_CELL_STATE_ERROR,        
} ml_cellular_state_t;

typedef enum {
    ML_DATA_MODE_NONE,          
    ML_DATA_MODE_PPP,           
    ML_DATA_MODE_AT_SOCKET,     
} ml_cellular_data_mode_t;





typedef struct {
    char imei[20];              
    char iccid[24];             
    char model[32];             
    char firmware[64];          
    char operator_name[32];     
    int rssi;                   
    int rssi_dbm;               
    bool sim_ready;             
    bool registered;            
    bool data_connected;        
} ml_cellular_info_t;













esp_err_t ml_cellular_init(const ml_cellular_config_t *config);




void ml_cellular_deinit(void);










esp_err_t ml_cellular_ppp_start(void);




esp_err_t ml_cellular_ppp_stop(void);









esp_err_t ml_cellular_data_start(void);





esp_err_t ml_cellular_data_stop(void);













esp_err_t ml_cellular_connect(void);





ml_cellular_data_mode_t ml_cellular_get_data_mode(void);





bool ml_cellular_is_data_mode(void);




ml_cellular_state_t ml_cellular_get_state(void);






esp_err_t ml_cellular_get_info(ml_cellular_info_t *info);





const char *ml_cellular_get_imei(void);











int ml_cellular_send_at(const char *cmd, char *response, size_t resp_size, int timeout_ms);

#ifdef __cplusplus
}
#endif
