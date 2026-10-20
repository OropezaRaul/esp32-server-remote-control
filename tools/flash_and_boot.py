#!/usr/bin/env python3
"""
Flashea el ESP32-S3 por USB nativo y lo deja ARRANCANDO la app.

Por que existe (2026-10-20, sesion 35):

El ESP32-S3 con USB nativo (USB-Serial/JTAG) ignora el reset por RTS/DTR de
esptool: 'esptool --after hard_reset' avisa 'Hard resetting via RTS pin...' y no
oye nada, asi que el chip se queda en el bootloader y no vuelve a levantar la app
(parece un brick: no responde ni por HTTP ni por Tailscale). La sesion 7b del
STATUS.md lo resolvia a mano (apretar RESET sin BOOT), lo cual no sirve para un
flasheo desatendido.

La secuencia que SI funciona con este peripheral es la de los estados de linea
control del USB CDC, en este orden exacto:

    DTR=1, RTS=1   (los dos a la vez: es el unico estado que el peripheral
                   propaga correctamente a EN/GPIO0)
    DTR=0         (IO0 en alto = arranque normal, no modo download)
    RTS=0         (se suelta EN: el chip arranca la app)

Ojo: al abrir el puerto con pyserial, DTR y RTS quedan en alto por defecto; hay
que fijarlos a False ANTES de abrir (dtr=False, rts=False) y luego hacer la
secuencia.

Este script tambien deja el puerto serie abierto durante el arranque: la
consola del firmware sale por el USB-Serial/JTAG, asi que se puede ver el log
del boot en directo (util para diagnosticar).

Uso:
    python3 tools/flash_and_boot.py [-p /dev/ttyACM0] [--console] 0x10000 app.bin

Requiere esptool (pip install esptool) y pyserial.
"""

import argparse
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    print("Falta pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(1)


def flash(port, extra):
    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", port,
        "--before", "usb_reset",
        "--after", "no_reset",
        "--baud", "460800",
    ] + extra
    print("+", " ".join(cmd))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print("El flasheo fallo", file=sys.stderr)
        sys.exit(r.returncode)


def boot_app(port):
    """Suelta el reset de forma que el chip arranque la app (no el bootloader)."""
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.3
    s.dtr = False
    s.rts = False
    s.open()
    try:
        s.setDTR(True)
        s.setRTS(True)
        time.sleep(0.2)
        s.setDTR(False)
        time.sleep(0.2)
        s.setRTS(False)
        time.sleep(0.5)
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("--console", action="store_true",
                    help="muestra el log del arranque por el puerto serie")
    ap.add_argument("files", nargs="*",
                    help="binarios a flashear (flash_addr=archivo)")
    args = ap.parse_args()

    if not args.files:
        print("Uso: flash_and_boot.py [-p PUERTO] flash_addr=archivo ...",
              file=sys.stderr)
        sys.exit(2)

    flash(args.port, ["write_flash"] + args.files)
    print("Flasheado. Arrancando la app...")
    boot_app(args.port)

    if args.console:
        print("--- consola del arranque (Ctrl-C para salir) ---")
        s = serial.Serial()
        s.port = args.port
        s.baudrate = 115200
        s.timeout = 0.3
        s.dtr = False
        s.rts = False
        s.open()
        try:
            while True:
                data = s.read(4096)
                if data:
                    sys.stdout.write(data.decode("utf-8", "replace"))
                    sys.stdout.flush()
        except KeyboardInterrupt:
            pass
        finally:
            s.close()
    else:
        print("Hecho. El chip deberia arrancar la app en ~15 s.")


if __name__ == "__main__":
    main()
