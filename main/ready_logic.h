#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>





























#define READY_LOGIC_FAST_MS   1000UL
#define READY_LOGIC_SLOW_MS  30000UL



#define READY_LOGIC_TTL_MS   60000UL


#define READY_LOGIC_ARM_MS  120000UL

typedef struct {
    bool     latched;          
    uint32_t latched_ms;       
    uint32_t arm_until_ms;     
    uint32_t last_probe_ms;    
    uint32_t probes;           
    uint32_t ok;               
    uint32_t err;              
    uint32_t last_ok_ms;       
    uint32_t ready_ms_ago;     
    uint32_t last_duration_ms; 
    int32_t  last_status;      
    int32_t  last_api_status;  
    uint32_t consec_fail;      
    bool     public_broken;    
                             
                             
                             
} ready_logic_t;


void ready_logic_init(ready_logic_t *l);



void ready_logic_arm(ready_logic_t *l, uint32_t now_ms, uint32_t hold_ms);


bool ready_logic_armed(const ready_logic_t *l, uint32_t now_ms);



bool ready_logic_due_probe(const ready_logic_t *l, uint32_t now_ms,
                           uint32_t fast_ms, uint32_t slow_ms);







void ready_logic_record(ready_logic_t *l, uint32_t now_ms, bool ok,
                        int32_t status, uint32_t dur_ms);


bool ready_logic_is_ready(const ready_logic_t *l, uint32_t now_ms, uint32_t ttl_ms);






void ready_logic_set_public_broken(ready_logic_t *l, bool broken);


void ready_logic_invalidate(ready_logic_t *l, uint32_t now_ms);



void ready_logic_format(const ready_logic_t *l, uint32_t now_ms,
                        char *out, size_t out_size);
