








#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "wd_logic.h"

static int g_checks = 0;
static int g_fail = 0;

static void expect(const char *name, int cond, long got, long want) {
    g_checks++;
    if (!cond) {
        g_fail++;
        printf("  FAIL %-46s got=%ld want=%ld\n", name, got, want);
    } else {
        printf("  ok   %s\n", name);
    }
}

static const char *act_name(wd_action_t a) {
    switch (a) {
        case WD_ACT_NONE:      return "NONE";
        case WD_ACT_PRE_WARN:  return "PRE_WARN";
        case WD_ACT_REBIND:    return "REBIND";
        case WD_ACT_REBOOT:    return "REBOOT";
    }
    return "?";
}


#define REBIND_MS  (150u * 1000u)
#define REBOOT_MS  (300u * 1000u)
#define COOLDOWN   (30u * 1000u)

static wd_input_t base_input(void) {
    wd_input_t in;
    memset(&in, 0, sizeof(in));
    in.ever_connected = true;
    in.state_connected = true;
    in.tick = 1000000u;               
    in.last_connected_ms = 1000000u - 4000u;  
    in.ctrl_stale_ms = 3000u;                 
    in.rebind_after_ms = REBIND_MS;
    in.reboot_after_ms = REBOOT_MS;
    in.cooldown_ms = COOLDOWN;
    return in;
}

int main(void) {
    printf("wd_logic tests\n");

    
    {
        wd_input_t in = base_input();
        in.ever_connected = false;
        wd_action_t a = wd_evaluate(&in);
        expect("1.1 no-armed -> NONE", a == WD_ACT_NONE, a, WD_ACT_NONE);
        expect("1.2 no-armed disconnect=0", in.disconnect_ms == 0, in.disconnect_ms, 0);
    }

    
    {
        wd_input_t in = base_input();
        wd_action_t a = wd_evaluate(&in);
        expect("2.1 healthy CONNECTED -> NONE", a == WD_ACT_NONE, a, WD_ACT_NONE);
        expect("2.2 healthy zombie=false", in.zombie == false, in.zombie, false);
        expect("2.3 healthy disconnect=3s", in.disconnect_ms == 3000u, in.disconnect_ms, 3000u);
    }

    
    {
        wd_input_t in = base_input();
        in.ctrl_stale_ms = 0;
        wd_action_t a = wd_evaluate(&in);
        expect("2b.1 CONNECTED no-RX-history -> NONE", a == WD_ACT_NONE, a, WD_ACT_NONE);
        expect("2b.2 disconnect=0", in.disconnect_ms == 0, in.disconnect_ms, 0);
    }

    
    {
        wd_input_t in = base_input();
        in.ctrl_stale_ms = 90u * 1000u;  
        in.last_connected_ms = 1000000u - 5000u;
        wd_action_t a = wd_evaluate(&in);
        expect("3.1 half-stale -> NONE", a == WD_ACT_NONE, a, WD_ACT_NONE);
        expect("3.2 zombie=true (>=60s)", in.zombie == true, in.zombie, true);
        expect("3.3 disconnect=90s", in.disconnect_ms == 90000u, in.disconnect_ms, 90000u);
    }

    
    {
        wd_input_t in = base_input();
        in.ctrl_stale_ms = 200u * 1000u;   
        wd_action_t a = wd_evaluate(&in);
        expect("4.1 zombie 200s -> REBIND", a == WD_ACT_REBIND, a, WD_ACT_REBIND);
        expect("4.2 zombie flag", in.zombie == true, in.zombie, true);
        
        in.rebind_done = true;
        a = wd_evaluate(&in);
        expect("4.3 zombie 200s after-rebind -> PRE_WARN", a == WD_ACT_PRE_WARN, a, WD_ACT_PRE_WARN);
    }

    
    {
        wd_input_t in = base_input();
        in.ctrl_stale_ms = 400u * 1000u;
        in.rebind_done = true;
        wd_action_t a = wd_evaluate(&in);
        expect("5.1 zombie 400s -> REBOOT", a == WD_ACT_REBOOT, a, WD_ACT_REBOOT);
    }

    
    {
        wd_input_t in = base_input();
        in.state_connected = false;                 
        in.last_connected_ms = in.tick - 100u * 1000u;
        in.ctrl_stale_ms = 200u * 1000u;            
        wd_action_t a = wd_evaluate(&in);
        expect("6.1 down 100s -> NONE", a == WD_ACT_NONE, a, WD_ACT_NONE);
        expect("6.2 zombie=false (down no es zombie)", in.zombie == false, in.zombie, false);
        expect("6.3 disconnect=100s", in.disconnect_ms == 100000u, in.disconnect_ms, 100000u);
    }

    
    {
        wd_input_t in = base_input();
        in.state_connected = false;
        in.last_connected_ms = in.tick - 200u * 1000u;
        wd_action_t a = wd_evaluate(&in);
        expect("7.1 down 200s -> REBIND", a == WD_ACT_REBIND, a, WD_ACT_REBIND);
        in.rebind_done = true;
        in.last_connected_ms = in.tick - 350u * 1000u;
        a = wd_evaluate(&in);
        expect("7.2 down 350s after rebind -> REBOOT", a == WD_ACT_REBOOT, a, WD_ACT_REBOOT);
    }

    
    {
        wd_input_t in = base_input();
        in.rebind_done = true;
        in.ctrl_stale_ms = 30u * 1000u;   
        wd_action_t a = wd_evaluate(&in);
        
        expect("8.1 fresh-RX -> NONE", a == WD_ACT_NONE, a, WD_ACT_NONE);
        expect("8.2 caller resets rebind window",
               in.disconnect_ms < in.rebind_after_ms, 1, 1);
    }

    
    {
        wd_input_t in = base_input();
        in.ctrl_stale_ms = 400u * 1000u;
        in.rebind_done = true;
        in.last_reboot_ms = in.tick - 10u * 1000u;  
        wd_action_t a = wd_evaluate(&in);
        expect("9.1 reboot cooldown -> NONE", a == WD_ACT_NONE, a, WD_ACT_NONE);
        
        in.last_reboot_ms = in.tick - 40u * 1000u;
        a = wd_evaluate(&in);
        expect("9.2 after cooldown -> REBOOT", a == WD_ACT_REBOOT, a, WD_ACT_REBOOT);
    }

    
    {
        wd_input_t in = base_input();
        in.state_connected = false;
        

        in.tick = 192u;
        in.last_connected_ms = (uint32_t)(in.tick - 256u * 1000u);
        wd_action_t a = wd_evaluate(&in);
        uint32_t diff = (uint32_t)(in.tick - in.last_connected_ms);
        expect("10.1 wrap diff=256s", diff == 256u * 1000u, diff, 256u * 1000u);
        expect("10.2 wrap 256s -> REBIND", a == WD_ACT_REBIND, a, WD_ACT_REBIND);
    }

    
    {
        wd_input_t in = base_input();
        in.state_connected = false;
        in.reboot_after_ms = 60u * 1000u;   
        in.rebind_after_ms = 60u * 1000u;   
        in.last_connected_ms = in.tick - 59u * 1000u;
        wd_action_t a = wd_evaluate(&in);
        
        expect("11.1 clamp -> PRE_WARN (no REBIND)", a == WD_ACT_PRE_WARN, a, WD_ACT_PRE_WARN);
        in.last_connected_ms = in.tick - 60u * 1000u;
        a = wd_evaluate(&in);
        expect("11.2 clamp at reboot -> REBOOT", a == WD_ACT_REBOOT, a, WD_ACT_REBOOT);
    }

    
    {
        wd_input_t in = base_input();
        in.state_connected = true;
        in.ctrl_stale_ms = 5u * 1000u;
        in.last_connected_ms = in.tick - 700u * 1000u;  
        in.rebind_done = true;
        wd_action_t a = wd_evaluate(&in);
        expect("12.1 reconnected -> NONE", a == WD_ACT_NONE, a, WD_ACT_NONE);
        expect("12.2 disconnect resets to ctrl window", in.disconnect_ms == 5000u, in.disconnect_ms, 5000u);
    }

    (void)act_name;
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
