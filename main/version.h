#pragma once

// Fuente ÚNICA de la versión del firmware.
//
// Por qué un header y no Kconfig: el símbolo CONFIG_FIRMWARE_VERSION queda
// "congelado" en sdkconfig cuando el símbolo ya existe con un valor previo —
// cambiar el default en Kconfig.projbuild NO lo promueve (gotcha documentado
// en STATUS.md sesiones 13/14/16: el banner dijo 1.4.8-ml-reboot durante las
// sesiones 15-17 aunque el default era 1.4.9-funnel-native). Un header
// siempre refleja el código compilado; nada que sincronizar.
//
// Al bumppear: editar SOLO la línea de abajo y recompilar.
#define FIRMWARE_VERSION "1.8.0-system-health-wdt"
