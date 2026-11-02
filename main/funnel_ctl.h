#pragma once





























#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>

#define FUNNEL_CTL_MAX_IPS 4
#define FUNNEL_CTL_IPTEXT_LEN 16

typedef struct {
    bool enabled;          
    bool healthy;          
    bool ever_ok;          

    uint32_t checks;       
    uint32_t failures;     
    uint32_t consecutive_failures;
    uint32_t repairs;      
    uint32_t last_check_ms_ago;  
    uint32_t last_ok_ms_ago;     
    char last_error[96];
    char ip[FUNNEL_CTL_MAX_IPS][FUNNEL_CTL_IPTEXT_LEN];
    uint8_t ip_count;
} funnel_ctl_status_t;


esp_err_t funnel_ctl_init(void);



esp_err_t funnel_ctl_set_enabled(bool on);



esp_err_t funnel_ctl_refresh(void);




bool funnel_ctl_check_now(char *detail, size_t detail_len);


void funnel_ctl_get_status(funnel_ctl_status_t *out);


void funnel_ctl_format_status(char *buf, size_t len);
