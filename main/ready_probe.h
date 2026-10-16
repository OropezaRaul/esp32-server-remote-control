#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>




































esp_err_t ready_init(void);



void ready_arm(void);



bool ready_is_ready(void);



void ready_notify_ready(void);






void ready_notify_unready(void);


bool ready_public_broken(void);


void ready_invalidate(void);


typedef struct {
    bool     latched;          
    bool     ready;            
    bool     armed;            
    uint32_t ready_ms_ago;     
    uint32_t probes, ok, err;  
    uint32_t last_duration_ms; 
    int32_t  last_status;      
    int32_t  last_api_status;  
} ready_status_t;

void ready_get_status(ready_status_t *out);


const char *ready_status_text(void);
