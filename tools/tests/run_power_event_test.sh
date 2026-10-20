#!/usr/bin/env bash
# Compila y corre el test host de power_event_logic (no requiere ESP-IDF).
# Prefiere clang si existe (algunos hosts tienen gcc sin cc1 instalado).
set -euo pipefail
cd "$(dirname "$0")/../.."    # repo root
if command -v clang >/dev/null 2>&1; then CC=clang; else CC=gcc; fi
"$CC" -Wall -Wextra -I main main/power_event_logic.c tools/tests/power_event_test.c -o /tmp/power_event_test
/tmp/power_event_test