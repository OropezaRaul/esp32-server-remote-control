







#include <stdio.h>
#include <string.h>

#include "ready_logic.h"

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_fail++;                                                        \
            printf("  FAIL %s:%d " fmt "\n", __FILE__, __LINE__,              \
                   ##__VA_ARGS__);                                          \
        }                                                                    \
    } while (0)

static void test_init(void) {
    ready_logic_t l;
    ready_logic_init(&l);
    CHECK(!ready_logic_is_ready(&l, 1000, READY_LOGIC_TTL_MS), "recien init no puede estar listo");
    CHECK(!ready_logic_armed(&l, 1000), "recien init nadie espera");
    CHECK(ready_logic_due_probe(&l, 1000, 1000, 30000), "el primer sondeo es siempre due");
    CHECK(l.last_status == -1, "last_status arranca en -1, no en 0 (=HTTP 0)");
    CHECK(!l.public_broken, "al arrancar no sabemos nada de la ruta publica");
}

static void test_cadence(void) {
    ready_logic_t l;
    ready_logic_init(&l);
    ready_logic_record(&l, 1000, false, -1, 30);
    CHECK(!ready_logic_due_probe(&l, 1000 + 29000, READY_LOGIC_FAST_MS, READY_LOGIC_SLOW_MS),
          "a los 29 s todavia no toca en mantenimiento");
    CHECK(ready_logic_due_probe(&l, 1000 + 30000, READY_LOGIC_FAST_MS, READY_LOGIC_SLOW_MS),
          "a los 30 s toca en mantenimiento");
    ready_logic_arm(&l, 40000, READY_LOGIC_ARM_MS);
    ready_logic_record(&l, 40000, false, -1, 400);
    CHECK(ready_logic_armed(&l, 40000), "arm abre la ventana de espera");
    CHECK(!ready_logic_due_probe(&l, 40000 + 999, READY_LOGIC_FAST_MS, READY_LOGIC_SLOW_MS),
          "a los 999 ms del ciclo rapido todavia no toca");
    CHECK(ready_logic_due_probe(&l, 40000 + 1000, READY_LOGIC_FAST_MS, READY_LOGIC_SLOW_MS),
          "a los 1000 ms toca: el requisito de 1 s");
}

static void test_arm_window(void) {
    ready_logic_t m;
    ready_logic_init(&m);
    ready_logic_arm(&m, 40000, READY_LOGIC_ARM_MS);
    CHECK(ready_logic_armed(&m, 40000 + READY_LOGIC_ARM_MS - 1),
          "la ventana sigue abierta hasta el final");
    CHECK(!ready_logic_armed(&m, 40000 + READY_LOGIC_ARM_MS + 1),
          "la ventana expira sola si nadie renueva");
    ready_logic_t r;
    ready_logic_init(&r);
    ready_logic_arm(&r, 40000, READY_LOGIC_ARM_MS);
    ready_logic_arm(&r, 40000 + 5000, READY_LOGIC_ARM_MS);
    CHECK(ready_logic_armed(&r, 40000 + READY_LOGIC_ARM_MS + 1),
          "renovar extiende la ventana (y nunca la acorta)");
    CHECK(!ready_logic_armed(&r, 40000 + 5000 + READY_LOGIC_ARM_MS + 1),
          "la ventana renovada expira cuando el cliente deja de pedir");
}

static void test_latch_and_ttl(void) {
    ready_logic_t l;
    ready_logic_init(&l);
    ready_logic_record(&l, 10000, false, 502, 120);
    CHECK(!ready_logic_is_ready(&l, 10100, READY_LOGIC_TTL_MS),
          "un 502 del Funnel NO es estar listo (era el bug de la sesion 32)");
    ready_logic_record(&l, 20000, true, 200, 42);
    CHECK(ready_logic_is_ready(&l, 20000, READY_LOGIC_TTL_MS), "un 200 enciende el latch");
    CHECK(ready_logic_is_ready(&l, 20000 + READY_LOGIC_TTL_MS, READY_LOGIC_TTL_MS),
          "el latch sigue vivo justo en el limite del TTL");
    CHECK(!ready_logic_is_ready(&l, 20000 + READY_LOGIC_TTL_MS + 1, READY_LOGIC_TTL_MS),
          "pasado el TTL el latch caduca (nada de 302 a un servidor caido)");
    ready_logic_record(&l, 20000 + 5000, false, -1, 1200);
    CHECK(ready_logic_is_ready(&l, 20000 + 5000, READY_LOGIC_TTL_MS),
          "un timeout aislado no debe apagar el latch");
    ready_logic_record(&l, 20000 + 6000, false, -1, 1200);
    CHECK(ready_logic_is_ready(&l, 20000 + 6000, READY_LOGIC_TTL_MS),
          "dos sondas seguidas tampoco apagan el latch: manda el poll de estado y el TTL");
    ready_logic_record(&l, 20000 + 7000, true, 200, 30);
    CHECK(ready_logic_is_ready(&l, 20000 + 7000, READY_LOGIC_TTL_MS),
          "una sonda buena despues de dos malas vuelve a encender el latch");
    ready_logic_record(&l, 20000 + 8000, false, -1, 100);
    ready_logic_invalidate(&l, 20000 + 9000);
    CHECK(!ready_logic_is_ready(&l, 20000 + 9000, READY_LOGIC_TTL_MS),
          "invalidate apaga el latch al instante");
    CHECK(l.last_ok_ms == 20000 + 7000, "invalidate conserva el historico del ultimo ok");
}

static void test_public_broken(void) {
    ready_logic_t l;
    ready_logic_init(&l);
    ready_logic_record(&l, 10000, true, 200, 30);
    CHECK(ready_logic_is_ready(&l, 10000, READY_LOGIC_TTL_MS), "arranca listo");
    

    ready_logic_set_public_broken(&l, true);
    CHECK(!ready_logic_is_ready(&l, 10000, READY_LOGIC_TTL_MS),
          "con la ruta publica rota el latch se apaga aunque la sonda fuera buena");
    ready_logic_record(&l, 12000, true, 200, 25);
    CHECK(!ready_logic_is_ready(&l, 12000, READY_LOGIC_TTL_MS),
          "una sonda posterior tampoco reabre el gate mientras el servidor diga roto");
    ready_logic_set_public_broken(&l, false);
    ready_logic_record(&l, 14000, true, 200, 25);
    CHECK(ready_logic_is_ready(&l, 14000, READY_LOGIC_TTL_MS),
          "el aviso de listo del servidor reabre el gate");
}

static void test_wrap(void) {
    ready_logic_t l;
    ready_logic_init(&l);
    uint32_t base = 0xFFFFF000UL;
    ready_logic_arm(&l, base, 5000);
    CHECK(ready_logic_armed(&l, base + 4000), "arm sobrevive al wrap de uint32");
    CHECK(!ready_logic_armed(&l, base + 6000), "arm expira tras el wrap");
    ready_logic_record(&l, base, true, 200, 30);
    ready_logic_record(&l, 0x00001000UL, true, 200, 31);
    CHECK(ready_logic_is_ready(&l, 0x00002000UL, READY_LOGIC_TTL_MS),
          "el latch sobrevive al wrap de uint32");
}

static void test_format(void) {
    ready_logic_t l;
    ready_logic_init(&l);
    char buf[128];
    ready_logic_record(&l, 5000, true, 200, 42);
    ready_logic_format(&l, 14000, buf, sizeof(buf));
    CHECK(strstr(buf, "OK") != NULL, "el texto dice OK: %s", buf);
    CHECK(strstr(buf, "9s") != NULL, "el texto dice la antiguedad: %s", buf);
    ready_logic_invalidate(&l, 15000);
    ready_logic_format(&l, 15000, buf, sizeof(buf));
    CHECK(strstr(buf, "inactivo") != NULL, "sin latch dice inactivo: %s", buf);
    ready_logic_arm(&l, 16000, READY_LOGIC_ARM_MS);
    ready_logic_format(&l, 17000, buf, sizeof(buf));
    CHECK(strstr(buf, "esperando") != NULL, "con alguien esperando dice esperando: %s", buf);
}

int main(void) {
    printf("ready_logic_test\n");
    test_init();
    test_cadence();
    test_arm_window();
    test_latch_and_ttl();
    test_public_broken();
    test_wrap();
    test_format();
    printf("%d checks, %d fallos\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
