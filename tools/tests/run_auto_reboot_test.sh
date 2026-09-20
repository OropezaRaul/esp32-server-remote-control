#!/usr/bin/env bash
# Compila y corre el test host de auto_reboot command parsing.
set -euo pipefail
cd "$(dirname "$0")/../.."
if command -v clang >/dev/null 2>&1; then CC=clang; else CC=gcc; fi
"$CC" -Wall -Wextra tools/tests/auto_reboot_test.c -o /tmp/auto_reboot_test
/tmp/auto_reboot_test
