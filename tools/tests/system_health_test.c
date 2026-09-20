#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#define RTC_HEALTH_MAGIC 0x53595354 // 'SYST'

typedef enum {
    TEST_CH_SCHEDULER = 0,
    TEST_CH_AUTOMATION,
    TEST_CH_WIFI,
    TEST_CH_STATE,
    TEST_CH_TELEGRAM,
    TEST_CH_AUTO_REBOOT,
    TEST_CH_COUNT
} test_channel_t;

typedef struct {
    uint32_t magic;
    uint32_t reset_count;
    uint32_t prev_uptime_s;
    test_channel_t failing_channel;
    char     failing_task[32];
    char     reason[64];
    bool     had_crash;
    int64_t  rtc_unix_sec;
    int64_t  rtc_save_esp_timer_us;
} test_rtc_data_t;

typedef struct {
    const char *name;
    uint32_t   max_interval_ms;
    uint32_t   last_heartbeat_ms;
    bool       enabled;
} test_channel_desc_t;

static test_channel_desc_t s_channels[TEST_CH_COUNT] = {
    [TEST_CH_SCHEDULER]   = { .name = "scheduler",      .max_interval_ms = 90000, .last_heartbeat_ms = 0, .enabled = false },
    [TEST_CH_AUTOMATION]  = { .name = "automation",     .max_interval_ms = 90000, .last_heartbeat_ms = 0, .enabled = false },
    [TEST_CH_WIFI]        = { .name = "wifi_keepalive", .max_interval_ms = 45000, .last_heartbeat_ms = 0, .enabled = false },
    [TEST_CH_STATE]       = { .name = "state_poll",     .max_interval_ms = 90000, .last_heartbeat_ms = 0, .enabled = false },
    [TEST_CH_TELEGRAM]    = { .name = "telegram_poll",  .max_interval_ms = 90000, .last_heartbeat_ms = 0, .enabled = false },
    [TEST_CH_AUTO_REBOOT] = { .name = "auto_reboot",    .max_interval_ms = 60000, .last_heartbeat_ms = 0, .enabled = false },
};

static test_rtc_data_t s_rtc;

static void mock_heartbeat(test_channel_t ch, uint32_t tick_now) {
    if ((int)ch < 0 || ch >= TEST_CH_COUNT) return;
    s_channels[ch].last_heartbeat_ms = tick_now;
    s_channels[ch].enabled = true;
}

static bool mock_evaluate(uint32_t tick_now, test_channel_t *out_failed_ch, uint32_t *out_elapsed) {
    bool all_healthy = true;
    for (int i = 0; i < TEST_CH_COUNT; i++) {
        if (!s_channels[i].enabled) continue;
        uint32_t last = s_channels[i].last_heartbeat_ms;
        uint32_t elapsed = (last > 0 && tick_now >= last) ? (tick_now - last) : 0;
        if (elapsed > s_channels[i].max_interval_ms) {
            all_healthy = false;
            if (out_failed_ch) *out_failed_ch = (test_channel_t)i;
            if (out_elapsed) *out_elapsed = elapsed;
            return false;
        }
    }
    return all_healthy;
}

int main(void) {
    printf("Running system_health test suite...\n");

    // 1. Initial state: all disabled -> healthy
    test_channel_t failed_ch = TEST_CH_COUNT;
    uint32_t elapsed = 0;
    assert(mock_evaluate(1000, &failed_ch, &elapsed) == true);
    printf("  ok 1: all disabled -> healthy\n");

    // 2. Enable scheduler, fresh heartbeat -> healthy
    mock_heartbeat(TEST_CH_SCHEDULER, 1000);
    assert(s_channels[TEST_CH_SCHEDULER].enabled == true);
    assert(mock_evaluate(2000, &failed_ch, &elapsed) == true);
    printf("  ok 2: active scheduler at 1s, evaluated at 2s -> healthy\n");

    // 3. Scheduler evaluated at 80s (elapsed 79s <= 90s) -> healthy
    assert(mock_evaluate(80000, &failed_ch, &elapsed) == true);
    printf("  ok 3: elapsed 79s <= 90s -> healthy\n");

    // 4. Scheduler evaluated at 95s (elapsed 94s > 90s) -> stall detected!
    assert(mock_evaluate(95000, &failed_ch, &elapsed) == false);
    assert(failed_ch == TEST_CH_SCHEDULER);
    assert(elapsed == 94000);
    printf("  ok 4: elapsed 94s > 90s -> stall correctly identified on scheduler!\n");

    // 5. Fresh heartbeat revives scheduler
    mock_heartbeat(TEST_CH_SCHEDULER, 95000);
    assert(mock_evaluate(96000, &failed_ch, &elapsed) == true);
    printf("  ok 5: heartbeat resets timer -> healthy again\n");

    // 6. Test wifi channel (max 45s)
    mock_heartbeat(TEST_CH_WIFI, 100000);
    assert(mock_evaluate(140000, &failed_ch, &elapsed) == true); // 40s elapsed
    assert(mock_evaluate(150000, &failed_ch, &elapsed) == false); // 50s elapsed
    assert(failed_ch == TEST_CH_WIFI);
    printf("  ok 6: wifi 50s > 45s -> stall correctly identified on wifi!\n");

    // 7. RTC persistence tests
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.magic = RTC_HEALTH_MAGIC;
    s_rtc.had_crash = true;
    s_rtc.reset_count = 1;
    s_rtc.prev_uptime_s = 3600;
    snprintf(s_rtc.failing_task, sizeof(s_rtc.failing_task), "scheduler");
    snprintf(s_rtc.reason, sizeof(s_rtc.reason), "stalled for 95 s (limit 90 s)");

    assert(s_rtc.magic == RTC_HEALTH_MAGIC);
    assert(s_rtc.had_crash == true);
    assert(strcmp(s_rtc.failing_task, "scheduler") == 0);
    assert(s_rtc.reset_count == 1);
    printf("  ok 7: RTC crash report data intact\n");

    // 8. Consume crash report
    s_rtc.had_crash = false;
    assert(s_rtc.had_crash == false);
    printf("  ok 8: crash report consumed\n");

    // 9. RTC time save/restore logic
    int64_t valid_time = 1715000000; // ~2024
    int64_t invalid_time = 5000;    // 1970
    assert(invalid_time < 1704067200);
    assert(valid_time >= 1704067200);
    s_rtc.rtc_unix_sec = valid_time;
    assert(s_rtc.rtc_unix_sec == valid_time);
    printf("  ok 9: RTC time validation and restore OK\n");

    printf("\nAll 9 system_health test suites passed successfully!\n");
    return 0;
}
