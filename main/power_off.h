// power_off.h — Secuencia unica de apagado de server.
//
// Por que existe este modulo (sesion 2026-10-20, ver STATUS.md):
// el apagado estaba implementado por TRES veces (telegram.c, automation.c y
// scheduler.c) con la misma logica y los MISMOS dos defectos:
//
//  1. Cuando server no respondia, el /off de Telegram contestaba
//     "OK: server not reachable (state UNKNOWN); assuming off." SIN
//     enviar nada. El servidor estaba en S3 (suspendido): su Tailscale se
//     caia al suspenderse y la maquina se quedaba encendida para siempre.
//     El usuario veia "OK" y el servidor jamas se apagaba.
//
//  2. El camino "S3 -> S0 -> S5" bloqueaba la tarea que lo llamaba hasta
//     ~90 s (30 vueltas de 2 s + sonda TCP). La tarea de Telegram tiene un
//     limite de latido de 90 s en el supervisor de salud: al superarlo, el
//     ESP32 se reiniciaba (esp_restart) en mitad del apagado y el bot se
//     quedaba mudo. La tarea de automation tiene el mismo limite.
//
// Este modulo centraliza la secuencia y la ejecuta con latidos:
//   * decide con la informacion real (TCP vivo / rastro de suspension),
//   * verifica el apagado por sonda cuando la respuesta HTTP se pierde,
//   * reporta latidos para que el supervisor de salud no reinicie el ESP32,
//   * ofrece una variante asincrona para que Telegram no se bloquee.

#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "system_health.h"

typedef enum {
    POWER_OFF_RESULT_OK = 0,        // el servidor quedo apagado de verdad
    POWER_OFF_RESULT_ALREADY_OFF,   // no respondio y no volvio tras el WOL
    POWER_OFF_RESULT_FAIL,          // no se pudo confirmar el apagado
} power_off_result_t;

typedef struct {
    power_off_result_t result;
    uint32_t elapsed_ms;
    bool     woke_up;        // hubo que despertarlo con WOL
    char     detail[112];    // detalle legible (sin acentos, va a Telegram)
} power_off_report_t;

#define POWER_OFF_STATUS_BUF_SIZE 224

// Ejecuta la secuencia completa de apagado. Es bloqueante (hasta ~100 s en el
// peor caso) pero llama a system_health_heartbeat(hb) durante las esperas,
// asi que el supervisor de salud no reinicia el ESP32 por una espera larga.
//
// hb debe ser el canal de la tarea que llama (HEALTH_CHANNEL_TELEGRAM,
// HEALTH_CHANNEL_AUTOMATION o HEALTH_CHANNEL_SCHEDULER).
esp_err_t power_off_run(const char *reason, health_channel_t hb,
                        power_off_report_t *rep);

// Igual que power_off_run(), pero en una task propia: el llamador (Telegram)
// recibe el control de inmediato y el resultado final se envia al chat_id
// indicado. Devuelve ESP_ERR_INVALID_STATE si ya hay un apagado en curso.
esp_err_t power_off_start_async(const char *reason, int64_t chat_id);

// Espera a que server deje de responder en :22 (o hasta timeout_ms).
// Se usa para confirmar un /suspend o un /off cuya respuesta HTTP se perdio
// (el daemon suspende/apaga la maquina ANTES de responder).
bool power_off_wait_until_down(uint32_t timeout_ms, health_channel_t hb);

// Estado legible de la ultima secuencia (para /diag).
void power_off_status_str(char *out, size_t out_size);
