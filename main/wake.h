#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>





























#define WAKE_RETRY_MS       10000UL   
#define WAKE_WINDOW_MS     240000UL   
#define WAKE_TICK_MS         1000UL   
#define WAKE_PROBE_MS        300      
#define WAKE_ALERT_COOLDOWN_MS (30UL * 60UL * 1000UL) 

typedef struct {
    bool     pending;
    uint32_t attempts;
    uint32_t elapsed_ms;
    uint32_t window_ms;
    uint32_t retry_ms;
    uint32_t last_send_failed;  
    char     text[96];          
} wake_status_t;



esp_err_t wake_init(void);




esp_err_t wake_request(const char *reason);



void wake_cancel(void);


void wake_get_status(wake_status_t *out);


const char *wake_status_text(void);
