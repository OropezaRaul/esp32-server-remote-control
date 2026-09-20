#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
gcc -Wall -Wextra -O2 -I"$SCRIPT_DIR/../../main" "$SCRIPT_DIR/system_health_test.c" -o "$SCRIPT_DIR/system_health_test"
"$SCRIPT_DIR/system_health_test"
rm -f "$SCRIPT_DIR/system_health_test"
