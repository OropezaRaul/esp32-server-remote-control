#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "wake_logic.h"

static int checks = 0;

static void check(bool cond, const char *what) {
    checks++;
    if (!cond) {
        printf("  FAIL: %s\n", what);
        exit(1);
    }
}

#define WINDOW_MS 240000UL
#define RETRY_MS  10000UL

int main(void) {
    printf("=== wake_logic: maquina de estados del intento de Auto-WOL ===\n");

    // 1. Estado inicial: nada en curso, primer envio inmediato.
    {
        wake_logic_t l;
        wake_logic_init(&l, 1000, WINDOW_MS);
        check(!wake_logic_pending(&l), "init: no pending");
        check(!wake_logic_due_send(&l, 1000, RETRY_MS), "init: no debe enviar sin request");
        check(wake_logic_elapsed_ms(&l, 5000) == 0, "init: elapsed 0 sin pending");
    }

    // 2. Primer request arma el episodio y el primer envio es INMEDIATO.
    //    (Este es el detalle que arregla el bug: el cliente no depende de
    //    recargar la pagina para que salga la primera rafaga.)
    {
        wake_logic_t l;
        wake_logic_init(&l, 0, WINDOW_MS);
        check(wake_logic_request(&l, 0, "app") == true, "request: arma episodio nuevo");
        check(wake_logic_pending(&l), "request: pending");
        check(strcmp(l.reason, "app") == 0, "request: reason guardado");
        check(wake_logic_due_send(&l, 0, RETRY_MS) == true, "request: 1er envio inmediato");

        wake_logic_record_send(&l, 0);
        check(l.attempts == 1, "record_send: intentos=1");
        check(wake_logic_due_send(&l, 9999, RETRY_MS) == false, "sin reintentar antes de los 10s");
        check(wake_logic_due_send(&l, 10000, RETRY_MS) == true, "reintenta a los 10s");
    }

    // 3. Requests repetidos NO duplican episodio ni estiran la ventana.
    //    Un cliente que recargue cada 2s no puede dejar al ESP32 emitiendo
    //    magic packets para siempre.
    {
        wake_logic_t l;
        wake_logic_init(&l, 0, WINDOW_MS);
        check(wake_logic_request(&l, 0, "app"), "r1: arma");
        wake_logic_record_send(&l, 0);
        check(wake_logic_request(&l, 2000, "app") == false, "r2: mismo episodio");
        check(l.attempts == 1, "r2: no reinicia intentos");
        check(wake_logic_request(&l, 60000, "gate") == false, "r3: mismo episodio");
        check(l.attempts == 1, "r3: no reinicia intentos");
        check(!wake_logic_expired(&l, 60000), "r3: ventana intacta a los 60s");
        // ...pero el motivo refleja el ultimo pedido (util para /diag).
        check(strcmp(l.reason, "gate") == 0, "r3: reason actualizado");
    }

    // 4. Confirmacion: el episodio termina en OK y queda el tiempo que tardo.
    {
        wake_logic_t l;
        wake_logic_init(&l, 0, WINDOW_MS);
        wake_logic_request(&l, 0, "app");
        wake_logic_record_send(&l, 0);
        wake_logic_record_send(&l, 10000);
        wake_logic_confirm(&l, 37000);
        check(!wake_logic_pending(&l), "confirm: pending=false");
        check(l.confirmed_ms == 37000, "confirm: timestamp");
        check(l.attempts == 2, "confirm: intentos conservados");
        check(!wake_logic_due_send(&l, 50000, RETRY_MS), "confirm: no envia mas");
        // Un request posterior abre un episodio NUEVO (limpio).
        check(wake_logic_request(&l, 50000, "app"), "post-confirm: rearma");
        check(l.attempts == 0, "post-confirm: intentos a 0");
        check(wake_logic_due_send(&l, 50000, RETRY_MS), "post-confirm: envio inmediato");
    }

    // 5. Expiracion: la ventana de 4 min cierra el intento.
    {
        wake_logic_t l;
        wake_logic_init(&l, 0, WINDOW_MS);
        wake_logic_request(&l, 0, "kiubart");
        wake_logic_record_send(&l, 0);
        check(!wake_logic_expired(&l, 239999), "expire: aun dentro");
        check(wake_logic_expired(&l, 240000), "expire: al completar 4 min");
        // Y tras expirar, un request nuevo arranca otro intento de verdad.
        wake_logic_fail(&l, 240000);
        check(!wake_logic_pending(&l), "fail: pending=false");
        check(l.failed_ms == 240000, "fail: timestamp");
        check(wake_logic_request(&l, 250000, "app"), "fail: rearma limpio");
        check(!wake_logic_expired(&l, 250000), "fail: ventana nueva completa");
    }

    // 6. Wrap del reloj de milisegundos (xTaskGetTickCount*portTICK_PERIOD_MS
    //    es uint32 y da la vuelta cada ~49.7 dias). Con aritmetica sin firmar
    //    la resta sigue dando el elapsed correcto...
    {
        wake_logic_t l;
        uint32_t t0 = 0xFFFFF000UL; 
        wake_logic_init(&l, t0, WINDOW_MS);
        wake_logic_request(&l, t0, "app");
        wake_logic_record_send(&l, t0);
        uint32_t later = t0 + 35000UL; 
        check(wake_logic_due_send(&l, later, RETRY_MS), "wrap: toca reintentar tras 35s");
        check(wake_logic_elapsed_ms(&l, later) == 35000, "wrap: elapsed correcto");
        check(!wake_logic_expired(&l, later), "wrap: no expira todavia");
    }

    // 7. reason defensivo: nunca debe desbordar el buffer.
    {
        wake_logic_t l;
        wake_logic_init(&l, 0, WINDOW_MS);
        wake_logic_request(&l, 0, "un motivo larguisimo que no cabe en el buffer");
        check(strlen(l.reason) < WAKE_LOGIC_REASON_LEN, "reason: truncado sin desbordar");
        wake_logic_request(&l, 1000, NULL);
        check(strcmp(l.reason, "wake") == 0, "reason: NULL -> wake");
    }

    // 8. Formato de estado legible para /diag.
    {
        wake_logic_t l;
        char buf[128];
        wake_logic_init(&l, 0, WINDOW_MS);
        wake_logic_request(&l, 0, "app");
        wake_logic_record_send(&l, 0);
        wake_logic_record_send(&l, 10000);
        wake_logic_format(&l, 25000, buf, sizeof(buf));
        check(strstr(buf, "PENDIENTE") != NULL, "format: pendiente");
        check(strstr(buf, "intentos=2") != NULL, "format: cuenta intentos");
        wake_logic_confirm(&l, 30000);
        wake_logic_format(&l, 30000, buf, sizeof(buf));
        check(strstr(buf, "OK en 30s") != NULL, "format: exito con segundos");
    }

    printf("=== %d checks OK ===\n", checks);
    return 0;
}
