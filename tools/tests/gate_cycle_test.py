#!/usr/bin/env python3
"""
Prueba del ciclo completo del gate del ESP32:
  1. POST /off o /suspend al servidor server (con el secreto compartido)
  2. Verificar que el servidor cae
  3. Entrar por la URL publica del Funnel del ESP32 (/app) y medir:
       - codigo HTTP (302 = gate abierto, 200 = splash "Despertando", 000 = caida)
       - tiempo de respuesta
  4. Esperar a que el servidor vuelva (ON) y confirmar que el gate da 302
  5. Repetir N veces

Uso: gate_cycle_test.py [off|suspend] [veces] [timeout_espera_s]
"""
import json, re, subprocess, sys, time, urllib.error, urllib.request

SECRET = re.search(r'CONFIG_SERVER_SHUTDOWN_SECRET="([^"]*)"',
                   open('/home/grim/Projects/Coding/ESP32_Remote_Control/sdkconfig').read()).group(1)
SHUTDOWN = "http://192.168.1.100:9999"
FUNNEL = "https://esp32-server.tailXXXX.ts.net/app"
DIAG = "http://192.168.1.50/diag"

def post(action):
    req = urllib.request.Request(f"{SHUTDOWN}/{action}", method="POST",
                                 headers={"X-ESP32-Secret": SECRET})
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            return r.status, r.read().decode()
    except Exception as e:
        return None, f"{type(e).__name__}: {e}"

def get_state():
    req = urllib.request.Request(f"{SHUTDOWN}/state", headers={"X-ESP32-Secret": SECRET})
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            return json.loads(r.read().decode())
    except Exception as e:
        return {"error": f"{type(e).__name__}: {e}"}

def diag():
    try:
        with urllib.request.urlopen(DIAG, timeout=8) as r:
            return json.loads(r.read().decode())
    except Exception as e:
        return {"error": f"{type(e).__name__}: {e}"}

def get_public_edge_ip():
    try:
        out = subprocess.check_output(["dig", "+short", "@1.1.1.1", "esp32-server.tailXXXX.ts.net", "A"],
                                      timeout=5).decode().strip().split()
        ips = [x for x in out if x and x[0].isdigit() and "." in x]
        return ips[0] if ips else None
    except Exception:
        return None

PUBLIC_IP = get_public_edge_ip()

def hit_funnel(timeout=30):
    """Devuelve (codigo, segundos, detalle). Usa la ruta publica real del Funnel con --resolve."""
    t0 = time.time()
    cmd = [
        "curl", "-sS", "-m", str(timeout), "-w", "\n__METRICS__ %{http_code} %{time_total}",
        "-A", "gate-cycle-test/1.0"
    ]
    if PUBLIC_IP:
        cmd += ["--resolve", f"esp32-server.tailXXXX.ts.net:443:{PUBLIC_IP}"]
    cmd.append(FUNNEL)

    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout + 5)
        out = res.stdout
        if "__METRICS__" in out:
            parts = out.rsplit("__METRICS__", 1)
            body = parts[0]
            meta = parts[1].strip().split()
            code = int(meta[0])
            dt = float(meta[1])
            if code == 0:
                det = f"err({res.returncode}): {res.stderr.strip()[:80]}"
            else:
                det = "SPLASH" if "Despertando" in body or "Iniciando" in body else ("302 Found" if code == 302 else body[:60].strip())
            return code, dt, det
        return 0, time.time() - t0, res.stderr[:80].strip() or "no metrics"
    except subprocess.TimeoutExpired:
        return 0, time.time() - t0, "subprocess timeout"
    except Exception as e:
        return 0, time.time() - t0, f"{type(e).__name__}: {e}"

def main():
    action = sys.argv[1] if len(sys.argv) > 1 else "suspend"
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    wait_s = int(sys.argv[3]) if len(sys.argv) > 3 else 180

    print(f"=== Ciclo de gate: /{action} x{rounds} (espera hasta {wait_s}s) ===")
    print(f"URL: {FUNNEL} (Edge public IP: {PUBLIC_IP})\n")
    resumen = []

    for r in range(1, rounds + 1):
        print(f"--- ronda {r}/{rounds} ---")
        st0 = get_state()
        print(f"  estado inicial: {st0.get('state')} uptime={st0.get('uptime_s')}")

        code, body = post(action)
        print(f"  POST /{action} -> {code} {body}")
        time.sleep(6)
        st1 = get_state()
        print(f"  estado tras la orden: {st1.get('state')}")

        # 1) Primera visita al gate con el servidor caido
        code, dt, det = hit_funnel()
        if code == 0:
            print(f"  gate #1 reintento tras fallo inicial [{det}]...")
            time.sleep(2)
            code, dt, det = hit_funnel()
        print(f"  gate #1 (servidor caido): HTTP {code} en {dt:.1f}s [{det}]")

        # 2) Esperar a que el servidor vuelva
        t0 = time.time()
        volvio = False
        while time.time() - t0 < wait_s:
            s = get_state()
            if s.get("state") == "ON":
                print(f"  servidor ON tras {time.time()-t0:.0f}s (uptime={s.get('uptime_s')})")
                volvio = True
                break
            time.sleep(3)
        if not volvio:
            print(f"  !! el servidor NO volvio en {wait_s}s")
            d = diag()
            print(f"  diag: wake={d.get('wake_status')} pending={d.get('wake_pending')} "
                  f"attempts={d.get('wake_attempts')} state={d.get('server_state')}")
            resumen.append((r, "TIMEOUT", code, dt))
            continue

        # 3) Segunda visita: con el servidor ya arriba, el gate debe dar 302
        code2, dt2, det2 = hit_funnel()
        print(f"  gate #2 (servidor arriba): HTTP {code2} en {dt2:.1f}s [{det2}]")

        # 4) Ráfaga de 5 visitas para medir estabilidad con el servidor ON
        codes = []
        for i in range(5):
            c, dt, d = hit_funnel()
            codes.append(c)
            time.sleep(1)
        ok = sum(1 for c in codes if c == 302)
        print(f"  rafaga x5 con el server ON: {codes} -> {ok}/5 con 302")
        resumen.append((r, "OK", code2, dt2, f"{ok}/5"))
        time.sleep(3)

    print("\n=== RESUMEN ===")
    for row in resumen:
        print("  ", row)

if __name__ == "__main__":
    main()
