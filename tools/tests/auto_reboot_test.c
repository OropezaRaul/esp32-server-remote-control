#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>

typedef enum {
    REBOOT_ACT_NOW,
    REBOOT_ACT_HELP,
    REBOOT_ACT_STATUS,
    REBOOT_ACT_SET,
    REBOOT_ACT_DISABLE,
    REBOOT_ACT_ERR,
} reboot_action_t;

static reboot_action_t parse_reboot_cmd(const char *args, uint32_t *out_val) {
    char clean[64] = {0};
    if (args != NULL) {
        while (*args == ' ' || *args == '\t') args++;
        size_t len = strlen(args);
        while (len > 0 && (args[len - 1] == ' ' || args[len - 1] == '\t' || args[len - 1] == '\r' || args[len - 1] == '\n')) {
            len--;
        }
        if (len >= sizeof(clean)) len = sizeof(clean) - 1;
        memcpy(clean, args, len);
        clean[len] = 0;
    }

    if (clean[0] == 0 || strcmp(clean, "now") == 0) {
        return REBOOT_ACT_NOW;
    }
    if (strcmp(clean, "help") == 0) {
        return REBOOT_ACT_HELP;
    }
    if (strcmp(clean, "status") == 0 || strcmp(clean, "info") == 0) {
        return REBOOT_ACT_STATUS;
    }
    if (strcasecmp(clean, "off") == 0 || strcasecmp(clean, "disable") == 0 || strcasecmp(clean, "disabled") == 0) {
        if (out_val) *out_val = 0;
        return REBOOT_ACT_DISABLE;
    }

    char *endptr = NULL;
    long val = strtol(clean, &endptr, 10);
    if (endptr != clean) {
        while (*endptr == ' ' || *endptr == '\t') endptr++;
        if (*endptr == 'h' || *endptr == 'H') {
            val *= 60;
            endptr++;
            while (*endptr == ' ' || *endptr == '\t') endptr++;
        } else if (*endptr == 'm' || *endptr == 'M') {
            endptr++;
            if (*endptr == 'i' || *endptr == 'I') endptr++;
            if (*endptr == 'n' || *endptr == 'N') endptr++;
            while (*endptr == ' ' || *endptr == '\t') endptr++;
        }

        if (*endptr == 0) {
            if (val < 0 || val > 10080) {
                return REBOOT_ACT_ERR;
            }
            if (val == 0) {
                if (out_val) *out_val = 0;
                return REBOOT_ACT_DISABLE;
            }
            if (out_val) *out_val = (uint32_t)val;
            return REBOOT_ACT_SET;
        }
    }
    return REBOOT_ACT_ERR;
}

int main(void) {
    uint32_t val = 999;
    printf("Running auto_reboot test suite...\n");

    // 1. Immediate reboot
    assert(parse_reboot_cmd(NULL, &val) == REBOOT_ACT_NOW);
    assert(parse_reboot_cmd("", &val) == REBOOT_ACT_NOW);
    assert(parse_reboot_cmd("   ", &val) == REBOOT_ACT_NOW);
    assert(parse_reboot_cmd("now", &val) == REBOOT_ACT_NOW);
    assert(parse_reboot_cmd("  now  ", &val) == REBOOT_ACT_NOW);

    // 2. Help
    assert(parse_reboot_cmd("help", &val) == REBOOT_ACT_HELP);
    assert(parse_reboot_cmd("  help  ", &val) == REBOOT_ACT_HELP);

    // 3. Status
    assert(parse_reboot_cmd("status", &val) == REBOOT_ACT_STATUS);
    assert(parse_reboot_cmd("  status  ", &val) == REBOOT_ACT_STATUS);
    assert(parse_reboot_cmd("info", &val) == REBOOT_ACT_STATUS);

    // 4. Disable (switch off)
    assert(parse_reboot_cmd("0", &val) == REBOOT_ACT_DISABLE && val == 0);
    assert(parse_reboot_cmd("  0  ", &val) == REBOOT_ACT_DISABLE && val == 0);
    assert(parse_reboot_cmd("off", &val) == REBOOT_ACT_DISABLE && val == 0);
    assert(parse_reboot_cmd("  OFF  ", &val) == REBOOT_ACT_DISABLE && val == 0);
    assert(parse_reboot_cmd("disable", &val) == REBOOT_ACT_DISABLE && val == 0);
    assert(parse_reboot_cmd("0h", &val) == REBOOT_ACT_DISABLE && val == 0);
    assert(parse_reboot_cmd("0m", &val) == REBOOT_ACT_DISABLE && val == 0);

    // 5. Setting minutes
    assert(parse_reboot_cmd("60", &val) == REBOOT_ACT_SET && val == 60);
    assert(parse_reboot_cmd("  30  ", &val) == REBOOT_ACT_SET && val == 30);
    assert(parse_reboot_cmd("1h", &val) == REBOOT_ACT_SET && val == 60);
    assert(parse_reboot_cmd("2h", &val) == REBOOT_ACT_SET && val == 120);
    assert(parse_reboot_cmd("45m", &val) == REBOOT_ACT_SET && val == 45);
    assert(parse_reboot_cmd("45min", &val) == REBOOT_ACT_SET && val == 45);
    assert(parse_reboot_cmd("10080", &val) == REBOOT_ACT_SET && val == 10080);

    // 6. Errors
    assert(parse_reboot_cmd("-5", &val) == REBOOT_ACT_ERR);
    assert(parse_reboot_cmd("10081", &val) == REBOOT_ACT_ERR);
    assert(parse_reboot_cmd("foo", &val) == REBOOT_ACT_ERR);
    assert(parse_reboot_cmd("60 abc", &val) == REBOOT_ACT_ERR);
    assert(parse_reboot_cmd("123zz", &val) == REBOOT_ACT_ERR);

    printf("All 27 assertions passed successfully!\n");
    return 0;
}
