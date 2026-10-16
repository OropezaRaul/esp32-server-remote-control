#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif





















typedef enum {
    WD_ACT_NONE = 0,
    WD_ACT_PRE_WARN,   

    
    WD_ACT_REBIND,     


    WD_ACT_REBOOT,     
} wd_action_t;

typedef struct {
    
    bool ever_connected;        
    bool state_connected;       
    uint32_t last_connected_ms; 
    uint32_t ctrl_stale_ms;     

    uint32_t tick;              
    uint32_t last_reboot_ms;    
    uint32_t last_rebind_ms;    
    bool rebind_done;           
    uint32_t reboot_after_ms;   
    uint32_t rebind_after_ms;   
    uint32_t cooldown_ms;       

    
    uint32_t disconnect_ms;     
    bool zombie;                

} wd_input_t;



wd_action_t wd_evaluate(wd_input_t *in);

#ifdef __cplusplus
}
#endif
