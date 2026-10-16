#include "wd_logic.h"
#include <stddef.h>



#define WD_ZOMBIE_REPORT_MS  60000u

wd_action_t wd_evaluate(wd_input_t *in) {
    if (in == NULL) return WD_ACT_NONE;
    uint32_t tick = in->tick;

    
    in->disconnect_ms = 0;
    in->zombie = false;

    

    if (!in->ever_connected) return WD_ACT_NONE;

    
    if (in->state_connected) {
        if (in->ctrl_stale_ms != 0) {
            



            in->disconnect_ms = in->ctrl_stale_ms;
            in->zombie = (in->ctrl_stale_ms >= WD_ZOMBIE_REPORT_MS);
        } else {
            
            in->disconnect_ms = 0;
        }
    } else {
        




        uint32_t anchor = in->last_connected_ms;
        if (anchor == 0) anchor = tick;   
        in->disconnect_ms = tick - anchor;
    }

    
    if (in->last_reboot_ms != 0 &&
        (tick - in->last_reboot_ms) < in->cooldown_ms) {
        return WD_ACT_NONE;
    }

    
    uint32_t reboot_ms = in->reboot_after_ms;
    uint32_t rebind_ms = in->rebind_after_ms;
    if (rebind_ms >= reboot_ms) rebind_ms = reboot_ms - 1u; 

    if (in->disconnect_ms >= reboot_ms) return WD_ACT_REBOOT;
    if (in->disconnect_ms >= rebind_ms && !in->rebind_done) return WD_ACT_REBIND;
    if (in->disconnect_ms >= (reboot_ms / 2u)) return WD_ACT_PRE_WARN;
    return WD_ACT_NONE;
}
