#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


















#define WAKE_LOGIC_REASON_LEN 24

typedef struct {
    bool     pending;            
    uint32_t armed_ms;           
    uint32_t last_request_ms;    
    uint32_t last_send_ms;       
    uint32_t window_ms;          
    uint32_t attempts;           
    uint32_t confirmed_ms;       
    uint32_t failed_ms;          
    char     reason[WAKE_LOGIC_REASON_LEN];
} wake_logic_t;




void wake_logic_init(wake_logic_t *l, uint32_t now_ms, uint32_t window_ms);



bool wake_logic_request(wake_logic_t *l, uint32_t now_ms, const char *reason);



bool wake_logic_due_send(const wake_logic_t *l, uint32_t now_ms, uint32_t retry_ms);


void wake_logic_record_send(wake_logic_t *l, uint32_t now_ms);


void wake_logic_confirm(wake_logic_t *l, uint32_t now_ms);



void wake_logic_fail(wake_logic_t *l, uint32_t now_ms);


bool wake_logic_expired(const wake_logic_t *l, uint32_t now_ms);

bool wake_logic_pending(const wake_logic_t *l);


uint32_t wake_logic_elapsed_ms(const wake_logic_t *l, uint32_t now_ms);


void wake_logic_format(const wake_logic_t *l, uint32_t now_ms,
                       char *out, size_t out_size);
