#!/usr/bin/env bash
# Compila y corre el test host de ready_logic (no requiere ESP-IDF).
# Prefiere clang si existe (algunos hosts tienen gcc sin cc1 instalado).
set -euo pipefail
cd "$(dirname "$0")/../.."    # repo root (Projects/Coding/ESP32_Remote_Control)
if command -v clang >/dev/null 2>&1; then CC=clang; else CC=gcc; fi
"$CC" -Wall -Wextra -I main main/ready_logic.c tools/tests/ready_logic_test.c -o /tmp/ready_logic_test
/tmp/ready_logic_test
