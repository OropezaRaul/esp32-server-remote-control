#!/usr/bin/env bash
#
# ready-notify.sh -- runs ON the server that the ESP32 gateway controls.
#
# WHAT IT DOES
# Watches the server own public URL and tells the gateway when that URL starts
# serving (GET /notify/ready) and when it stops serving (GET /notify/unready).
# It never waits a fixed amount of time: it polls and reports what it measures.
#
# WHY THE GATEWAY CANNOT DO THIS ALONE
# The gateway verifies the backend over the LAN, which is what the reverse proxy
# forwards to, but it cannot tell whether the proxy or the tunnel in front of that
# backend is up. Whoever sits next to the tunnel is this machine, so the end-to-end
# check belongs here. The gateway keeps probing on its own, so it works fine with
# or without this script.
#
# CONFIG (/etc/ready-notify.env):
#   READY_SECRET=...            required, same value as CONFIG_ESP32_READY_NOTIFY_SECRET
#   ESP32_TS_IP=100.64.0.2      gateway tailnet IP (preferred)
#   ESP32_LAN_IP=192.168.1.100  gateway LAN IP (fallback)
#   READY_URL=https://server.tailXXXX.ts.net/app/
#   READY_RESOLVE=host:443:IP   pin the name to the tailnet IP (see note below)
#   READY_TIMEOUT_S=300         budget for the initial wait
#   STEADY_POLL_S=5             watch cadence once confirmed
#
set -u

READY_URL="${READY_URL:-https://server.tailXXXX.ts.net/app/}"
ESP32_TS_IP="${ESP32_TS_IP:-100.64.0.2}"
ESP32_LAN_IP="${ESP32_LAN_IP:-192.168.1.100}"
READY_TIMEOUT_S="${READY_TIMEOUT_S:-300}"
STEADY_POLL_S="${STEADY_POLL_S:-5}"
READY_RESOLVE="${READY_RESOLVE:-}"
MARKER=/run/server-ready

log() { echo "[$(date -Is)] $*"; }

# --resolve is REQUIRED when probing from the server itself: the MagicDNS name
# resolves there to the PUBLIC Funnel IP (and to IPv6), so curl walks out to the
# internet and never comes back (HTTP 000). Pinning host:443 to the tailnet IP
# verifies what actually matters: the Funnel listener, the TLS handshake and the
# app vhost.
probe() {
  # HEAD first (does not render the page); fall back to GET if unsupported.
  local code
  local resolve_args=()
  if [ -n "${READY_RESOLVE}" ]; then
    resolve_args=(--resolve "${READY_RESOLVE}")
  fi
  code=$(curl "${resolve_args[@]}" -s -o /dev/null -I -m 5 -w "%{http_code}" "$READY_URL" 2>/dev/null || echo 000)
  case "$code" in
    2*|3*) echo "$code"; return 0 ;;
  esac
  code=$(curl "${resolve_args[@]}" -s -o /dev/null -m 5 -w "%{http_code}" "$READY_URL" 2>/dev/null || echo 000)
  echo "$code"
}

# Backend health. The SPA shell (nginx -> frontend) starts BEFORE FastAPI, so
# there is a window where the shell answers 200 while the API has no upstream
# yet and returns 502. The web app would open in a broken state during that
# window. This check makes the readiness announcement wait for the API too.
api_ok() {
  local url="${READY_URL%/}/api/v1/health"
  local code
  local resolve_args=()
  if [ -n "${READY_RESOLVE}" ]; then
    resolve_args=(--resolve "${READY_RESOLVE}")
  fi
  code=$(curl "${resolve_args[@]}" -s -o /dev/null -m 5 -w "%{http_code}" "$url" 2>/dev/null || echo 000)
  case "$code" in
    2*|3*) return 0 ;;
  esac
  return 1
}

is_up() {
  case "$1" in
    2*|3*) return 0 ;;
  esac
  return 1
}

notify() {
  local path="$1"
  local out=""
  for target in "$ESP32_TS_IP" "$ESP32_LAN_IP"; do
    [ -n "$target" ] || continue
    out=$(curl -s -m 3 -H "X-ESP32-Ready-Secret: ${READY_SECRET}" \
          "http://${target}${path}" 2>/dev/null || true)
    case "$out" in
      *"latch encendido"*) log "gateway notified ($path) via ${target}"; return 0 ;;
      *"latch apagado"*)   log "gateway notified ($path) via ${target}"; return 0 ;;
      *"401"*|*"503"*)      log "gateway ${target} rejected the notice: ${out:0:60}"; return 1 ;;
    esac
    log "gateway ${target} did not answer (${out:-no reply}); trying the next one"
  done
  return 1
}

if [ -z "${READY_SECRET:-}" ]; then
  log "READY_SECRET missing from /etc/ready-notify.env: not notifying (the gateway still probes on its own)"
  exit 0
fi

rm -f "$MARKER"
T0=$(date +%s)
log "waiting for the stack to really serve ($READY_URL), 1 s per attempt, cap ${READY_TIMEOUT_S}s"

up=0
code=000
while [ $(( $(date +%s) - T0 )) -lt "$READY_TIMEOUT_S" ]; do
  code=$(probe)
  if is_up "$code" && api_ok; then
    log "ready: HTTP $code + API ok after $(( $(date +%s) - T0 )) s since this service started"
    date -Is > "$MARKER"
    notify /notify/ready || log "could not notify: the gateway will detect it with its own probe"
    up=1
    break
  fi
  sleep 1
done

if [ "$up" != "1" ]; then
  log "TIMEOUT: ${READY_TIMEOUT_S}s and the public URL is still not serving (last HTTP $code)"
  exit 0
fi

log "watching the public URL every ${STEADY_POLL_S}s (on failure: /notify/unready)"
while true; do
  sleep "$STEADY_POLL_S"
  code=$(probe)
  if is_up "$code" && api_ok; then
    if [ "$up" != "1" ]; then
      log "the public URL serves again (HTTP $code + API ok)"
      date -Is > "$MARKER"
      notify /notify/ready
      up=1
    fi
  else
    if [ "$up" = "1" ]; then
      log "the public URL STOPPED serving (SPA $code): notifying the gateway"
      rm -f "$MARKER"
      notify /notify/unready
      up=0
    fi
  fi
done
