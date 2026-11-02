#include "sockmgr.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

static const char *TAG = "sockmgr";






static SemaphoreHandle_t s_pool = NULL;
static volatile int s_in_use = 0;
static volatile uint32_t s_enfile = 0;

bool sockmgr_take(uint32_t timeout_ms) {
    if (s_pool == NULL) {
        s_pool = xSemaphoreCreateCounting(CONFIG_SOCKMGR_MAX_TRANSIENT,
                                          CONFIG_SOCKMGR_MAX_TRANSIENT);
        if (s_pool == NULL) {
            ESP_LOGE(TAG, "no se pudo crear el semaforo del presupuesto");
            return true;   
        }
    }
    if (timeout_ms == 0) timeout_ms = CONFIG_SOCKMGR_WAIT_MS;

    if (xSemaphoreTake(s_pool, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "presupuesto de sockets transitorios LLENO (%d): "
                      "la operacion se forego (el camino publico tiene "
                      "reservados sus huecos)", CONFIG_SOCKMGR_MAX_TRANSIENT);
        return false;
    }
    s_in_use++;
    return true;
}

void sockmgr_give(void) {
    if (s_pool == NULL) return;
    if (s_in_use > 0) s_in_use--;
    xSemaphoreGive(s_pool);
}

int sockmgr_free(void) {
    if (s_pool == NULL) return CONFIG_SOCKMGR_MAX_TRANSIENT;
    return CONFIG_SOCKMGR_MAX_TRANSIENT - s_in_use;
}

int sockmgr_in_use(void) {
    return s_in_use;
}

uint32_t sockmgr_enfile_hits(void) {
    return s_enfile;
}

void sockmgr_note_enfile(void) {
    s_enfile++;
    ESP_LOGW(TAG, "ENFILE: la tabla de sockets de lwIP esta LLENA "
                  "(acumulado %u)", (unsigned)s_enfile);
}

void sockmgr_format_status(char *buf, size_t len) {
    if (!buf || len == 0) return;
    snprintf(buf, len,
             "sockets: transitorios %d/%d (libres %d) | ENFILE: %u",
             s_in_use, CONFIG_SOCKMGR_MAX_TRANSIENT, sockmgr_free(),
             (unsigned)s_enfile);
}
