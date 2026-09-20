# wd_logic — tests host del core del watchdog

`wd_logic.c/h` es el módulo PURO de decisión del watchdog de Tailscale: sin
dependencias de ESP-IDF/FreeRTOS, para poder testear el algoritmo en host
con gcc/clang (no hace falta hardware).

## Ejecutar

```bash
bash tools/tests/run_wd_test.sh
```

El script elige clang si existe (algunos hosts tienen gcc sin cc1 instalado),
compila `main/wd_logic.c` + `tools/tests/wd_logic_test.c` y corre los checks.

## Qué cubre (29 checks, escenarios de `wd_evaluate`)

- Watchdog sin armar (nunca vio CONNECTED) → jamás actúa
- CONNECTED sano / recién conectado sin historial de RX → NONE
- Semizombie (RX viejo pero < umbral rebind) → NONE + flag zombie
- Zombie CONNECTED (silencio del control plane):
  - 200s → REBIND (1x por episodio, luego PRE_WARN)
  - 400s → REBOOT
- RECONNECTING/WIFI_WAIT medidos desde el último CONNECTED
- Ventana de rebind reseteada cuando vuelve la vida fresca
- Antirebote de REBOOT (cooldown 30s)
- Wrap del contador uint32 (~49.7 días de uptime)
- Clamp rebind < reboot (misconfiguración)

## Nota para mantenerlo sincronizado

Si se toca la lógica de `wd_evaluate()` (umbrales, escalera, métricas),
actualizar también estas pruebas. El test host no sustituye la prueba en
campo (ver STATUS.md sesión 21/22): detectar la sesión zombie requiere un
corte de enlace real en el dispositivo.

## Gotcha de tooling (ver STATUS.md sesión 22)

Para editar código C desde el agente usar `String.raw` en el template
literal (o array de líneas + join): un template normal convierte `\n` en
salto de línea real y rompe los strings C. Verificar con
`grep -n '^"' <archivo>` si algo se ve raro.
