#!/usr/bin/env bash
# Helper: inicializa el entorno ESP-IDF v5.3 y ejecuta idf.py con los argumentos dados.
# Uso: ./idf.sh build
#      ./idf.sh -p /dev/ttyACM0 flash monitor
#      ./idf.sh menuconfig
#
# Si existe sdkconfig.credentials, sus lineas no-comentadas se anexan al
# sdkconfig (gitignored) despues de `reconfigure`, para que los secretos
# (WiFi pass, Tailscale key, bot token, shutdown secret) tengan efecto.
#
# Busca ESP-IDF en este orden:
#   1. $IDF_PATH (si ya está seteado)
#   2. ~/esp/esp-idf (instalación estándar)
#   3. ~/.espressif/esp-idf (alternativa común)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# --- Encontrar ESP-IDF ---
find_idf() {
    if [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/export.sh" ]]; then
        echo "$IDF_PATH"
        return
    fi
    for candidate in "$HOME/esp/esp-idf" "$HOME/.espressif/esp-idf"; do
        if [[ -f "$candidate/export.sh" ]]; then
            echo "$candidate"
            return
        fi
    done
    return 1
}

IDF_DIR="$(find_idf)" || {
    echo -e "\033[31m[ERROR] ESP-IDF no encontrado. Instalar con:\033[0m"
    echo "  mkdir -p ~/esp && cd ~/esp"
    echo "  git clone -b v5.3 --recursive https://github.com/espressif/esp-idf.git"
    echo "  cd esp-idf && ./install.sh"
    echo ""
    echo "O setear IDF_PATH apuntando a tu instalación existente."
    exit 1
}

export IDF_PATH="$IDF_DIR"

# --- Source export.sh (capturar solo las variables IDF, no todo el entorno) ---
ENV_DUMP=$(bash -c "source '$IDF_PATH/export.sh' > /dev/null 2>&1 && env" 2>/dev/null)

while IFS='=' read -r name value; do
    case "$name" in
        IDF_*|PATH)
            export "$name=$value"
            ;;
    esac
done <<< "$ENV_DUMP"

if ! command -v idf.py &>/dev/null; then
    echo -e "\033[31m[ERROR] idf.py no está en PATH tras sourcing ESP-IDF.\033[0m"
    exit 1
fi

# --- Auto-detectar puerto serial si no se pasó -p ---
SERIAL_PORT=""
for port in /dev/ttyACM0 /dev/ttyUSB0; do
    if [[ -e "$port" ]]; then
        SERIAL_PORT="$port"
        break
    fi
done

# Si el usuario no pasó -p y detectamos un puerto, inyectarlo
ARGS=("$@")
HAS_PORT=false
for arg in "${ARGS[@]}"; do
    if [[ "$arg" == "-p" || "$arg" == "--port" ]]; then
        HAS_PORT=true
        break
    fi
done

if [[ "$HAS_PORT" == false && -n "$SERIAL_PORT" ]]; then
    # Solo inyectar para comandos que lo necesitan
    case "${ARGS[0]:-}" in
        flash|monitor|flash_monitor)
            ARGS=("-p" "$SERIAL_PORT" "${ARGS[@]}")
            echo -e "\033[36m[auto] Puerto serial detectado: $SERIAL_PORT\033[0m"
            ;;
    esac
fi

# --- Merge sdkconfig.credentials ---
if [[ -f "sdkconfig.credentials" ]]; then
    FIRST_CMD="${ARGS[0]:-}"
    case "$FIRST_CMD" in
        reconfigure|set-target|menuconfig|build|fullclean)
            echo -e "\033[36m[merge] aplicando sdkconfig.credentials -> sdkconfig\033[0m"
            {
                echo ""
                echo "# === from sdkconfig.credentials ==="
                grep -v '^\s*#' sdkconfig.credentials | grep -v '^\s*$'
            } >> sdkconfig
            ;;
    esac
fi

# --- Ejecutar idf.py ---
idf.py "${ARGS[@]}"
