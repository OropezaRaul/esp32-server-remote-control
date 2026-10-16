#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

































#define ML_WD_RECOVERY_COOLDOWN_MS  30000

typedef enum {
    ML_WD_RECOVERY_NONE = 0,
    ML_WD_RECOVERY_REBOOT,           
    ML_WD_RECOVERY_REBIND,           
} microlink_wd_recovery_kind_t;


typedef struct {
    bool enabled;
    bool ever_connected;               
    uint32_t disconnect_ms;            

    uint32_t ctrl_rx_ms_ago;           

    uint32_t total_recoveries;         
    uint32_t total_rebinds;            
    microlink_wd_recovery_kind_t last_recovery_kind;
    uint32_t last_recovery_ms_ago;     
    uint32_t last_recovery_target_s;   
    uint32_t last_rebind_ms_ago;       
} microlink_wd_status_t;



esp_err_t microlink_watchdog_init(void);


void microlink_watchdog_get_status(microlink_wd_status_t *out);



esp_err_t microlink_watchdog_force_recover(void);



void microlink_watchdog_format_status(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
