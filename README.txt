==============================================================================
ESP32-S3 SERVER REMOTE CONTROL & AUTO-WOL GATEWAY (v1.9.0)
==============================================================================

An autonomous, 24/7 out-of-band management gateway running on ESP32-S3.
Controls, wakes, monitors, and automates your home or office server via
Telegram and Tailscale (WireGuard) without opening router ports or exposing
your private IP to the public internet.


------------------------------------------------------------------------------
TABLE OF CONTENTS
------------------------------------------------------------------------------
1. What Problem Does This Project Solve?
2. Hardware Requirements & Supported Devices
3. Feature Overview
4. Telegram Commands Reference
5. System Architecture & Reliability Engineering
6. Setup & Flashing Instructions
7. Server-Side Daemon Setup (Python/systemd)
8. Public Release Changelog
9. Acknowledgments & Credits
10. License


------------------------------------------------------------------------------
1. WHAT PROBLEM DOES THIS PROJECT SOLVE?
------------------------------------------------------------------------------
Keeping a powerful desktop or home server powered on 24/7 consumes significant
electricity and produces unnecessary heat. However:
- Putting a server to sleep (ACPI S3 Suspend-to-RAM) or shutting it down (S5)
  makes it unreachable from outside the local network.
- Standard Wake-on-LAN (WOL) magic packets cannot cross the internet through
  CGNAT or NAT firewalls without port forwarding or VPNs.
- Typical software watchdogs fail when network tasks deadlock, sockets hang,
  or memory runs low, freezing the management controller.

This project turns a low-cost, low-power (~0.3W) ESP32-S3 into a bulletproof
management controller that:
- Maintains an encrypted, zero-configuration WireGuard tunnel to your Tailscale
  network (via MicroLink v2).
- Provides an interactive Telegram Bot for remote power control from anywhere.
- Sends Wake-on-LAN magic packets using multiple broadcast and unicast vectors
  (including static ARP table bypass for router isolation).
- Automatically manages inactivity sleep (S3) and power off (S5) with
  protected quiet hours.
- Incorporates a mission-critical System Health Supervisor that monitors all
  tasks with cooperative heartbeats and hardware watchdog (TWDT) enforcement.
- Automatically captures logs into PSRAM and RTC memory, sending crash reports
  and diagnostic logs directly to Telegram as plain text (.txt) files.


------------------------------------------------------------------------------
2. HARDWARE REQUIREMENTS & SUPPORTED DEVICES
------------------------------------------------------------------------------
[+] Reference Hardware Tested & Validated:
    - Chip: Espressif ESP32-S3 (Xtensa dual-core 32-bit LX7 @ 240 MHz).
    - Exact Module: ESP32-S3-WROOM-1-N16R8 (or DevKitC-1 N16R8).
    - Flash Memory: 16 MB SPI Flash (Quad/Octal SPI, DIO mode, 80 MHz).
    - PSRAM: 8 MB Octal PSRAM (OPI mode, 80 MHz, 3.3V).
    - USB Interface: Native USB-Serial/JTAG (CDC ACM).

[+] Why PSRAM is Mandatory:
    - MicroLink WireGuard engine: Peer tables and cryptographic state.
    - mbedTLS TLS stack: HTTPS client for Telegram Bot API and web server.
    - Live Log Ring Buffer: 64 KB rolling log buffer in external memory.
    - A minimum of 2 MB PSRAM is required; 8 MB Octal PSRAM is strongly
      recommended for optimal performance.

[+] Other Compatible Hardware & Boards:
    - ESP32-S3-DevKitC-1-N8R8 (8 MB Flash, 8 MB Octal PSRAM).
    - ESP32-S3-DevKitC-1-N16R8 (16 MB Flash, 8 MB Octal PSRAM).
    - Waveshare ESP32-S3-Dev-Kit-N8R8 / N16R8.
    - LilyGO T-Display-S3 / T-SIM7080G-S3 (variants with PSRAM).
    - Seeed Studio XIAO ESP32S3 (8 MB Flash, 8 MB PSRAM).
    - ESP32-WROVER-E / WROVER-IE (Original ESP32 dual-core LX6 with 8 MB
      Quad PSRAM - requires changing target in idf.py to esp32 and pinouts).


------------------------------------------------------------------------------
3. FEATURE OVERVIEW
------------------------------------------------------------------------------
- Tailscale VPN (MicroLink v2):
  Direct WireGuard tunnel using Curve25519, ChaCha20-Poly1305, and BLAKE2s.
  Connects to DERP relay servers and uses STUN for peer-to-peer hole punching.

- Multi-Destination Wake-on-LAN:
  Sends magic packets via 4 simultaneous vectors:
  1) Subnet broadcast (e.g. 192.168.1.255:9)
  2) Echo port broadcast (port 7)
  3) Universal broadcast (255.255.255.255:9 and :7)
  4) Direct unicast with static ARP table injection (bypasses AP isolation).

- Evidence-Based Readiness Gate (no sleep timers):
  The gateway never waits a fixed amount of time before forwarding a client to
  the server. A background task probes the real destination and publishes a
  latch as soon as it answers:
    1) the web front end (SPA shell) responds, and
    2) the backend health endpoint responds.
  Polling runs at 1 s while somebody is waiting on the splash screen and 30 s
  as idle maintenance. A previous design waited a hard-coded 45 s after the
  server was seen "ON", which cost ~25 s of dead waiting on cold boots and could
  never adapt to how fast a machine actually boots.

- Server-Side Readiness Push:
  The server can verify its OWN public URL and announce readiness to the gateway
  (GET /notify/ready, and /notify/unready when the public path stops serving).
  A systemd unit is included under tools/. It closes the gap the ESP32 cannot
  observe on its own: the gateway can see the backend over the LAN, but not
  whether the reverse proxy or tunnel in front of it is up. Works with or
  without it: the probe alone is enough.

- Auto-WOL Supervisor:
  A gateway visit arms a wake attempt in firmware (not in the browser): magic
  packet immediately, then retried every 10 s for up to 4 minutes, stopping as
  soon as the server answers and alerting over Telegram if the window expires.

- Interactive Telegram Bot:
  Full command suite for server management, diagnostics, and log export.
  Access restricted by whitelist of authorized Telegram chat IDs.

- System Health Supervisor & Hardware Watchdog:
  Internal heartbeat matrix monitoring all critical FreeRTOS tasks.
  Directly coupled to ESP-IDF Task Watchdog Timer (TWDT).

- Black-Box RTC Memory Crash Logger:
  Preserves pre-crash log lines across reboots in RTC Fast Memory.
  Automatically sends crash_log.txt to Telegram upon recovery.

- Inactivity Automation:
  Configurable auto-suspend (S3) and auto-off (S5) timers with protected
  quiet hours (e.g. 08:00 to 18:00) during working periods.

- Daily Scheduler with RTC Clock Resilience:
  Scheduled daily wake and off times. Backed up by RTC clock memory to ensure
  timely execution even if power or internet is briefly interrupted.


------------------------------------------------------------------------------
4. TELEGRAM COMMANDS REFERENCE
------------------------------------------------------------------------------
/wake
  Sends the Wake-on-LAN multi-destination burst to power on the server.

/status
  Quick status report without blocking TCP calls:
  Reports server power state (ON / SUSPEND / OFF), uptime, free internal heap,
  and PSRAM free memory.

/stats
  Queries the server-side monitor daemon (port 8085) and displays live system
  metrics: CPU usage, RAM utilization, disk space, and Docker container state.

/ping
  Performs an active live TCP probe to the server (port 22) to verify network
  responsiveness.

/diag
  Deep system diagnostics:
  - Internal heap (free / min ever / largest block)
  - PSRAM utilization
  - Active FreeRTOS task count
  - WiFi status and IP
  - Tailscale VPN connection status and peer IP
  - Auto-reboot timer countdown
  - System Health Supervisor heartbeat matrix (status and latency per task)

/logs
  Exports the live rolling log buffer (last 64 KB) directly to your Telegram
  chat as a plain text file attachment (esp32_log.txt). Ideal for offline
  inspection or feeding to AI coding agents.

/crashlog
  Exports the black-box crash log from RTC Fast Memory as crash_log.txt.

/off
  Sends authenticated POST /off (S5 power down) to the server. If the server
  is in S3 sleep (NIC down), it automatically executes the ACPI transition:
  S3 -> S0 (WOL) -> S5 (/off).

/suspend
  Sends authenticated POST /suspend (ACPI S3 Suspend-to-RAM) to the server.

/schedule
  Manages daily automated wake and off times:
  - /schedule                 -> Displays current schedule.
  - /schedule set wake HH:MM  -> Sets daily wake time (e.g. /schedule set wake 07:00).
  - /schedule set off  HH:MM  -> Sets daily power off time.
  - /schedule reset           -> Clears all schedules.

/reboot
  Manages ESP32 rebooting:
  - /reboot now               -> Reboots the ESP32 immediately.
  - /reboot <min>             -> Sets periodic auto-reboot interval in minutes.
  - /reboot off               -> Disables periodic auto-reboot.

/automation
  Displays inactivity automation status: idle timers, current countdowns,
  and protected quiet hours.

/autoset <key> <val>
  Configures automation parameters at runtime without reflashing:
  - /autoset enabled 0|1      -> Enable/disable inactivity automation.
  - /autoset suspend <min>    -> Minutes of inactivity before S3 suspend.
  - /autoset off <min>        -> Minutes in S3 before S5 power off.
  - /autoset quiet HH:MM-HH:MM-> Protected hours where auto-off is suppressed.
  - /autoset defaults         -> Resets automation to Kconfig defaults.

/portmap
  Manages dynamic TCP port forwarding and reverse proxies through the ESP32:
  - /portmap list             -> Lists active mappings.
  - /portmap add <local> <upstream> [host] [proto] [label]
  - /portmap del <local>      -> Removes a mapping.
  - /portmap defaults         -> Restores default mappings.

/listauth
  Displays the list of authorized Telegram chat IDs.

/help
  Displays the command menu and quick usage instructions.


------------------------------------------------------------------------------
5. SYSTEM ARCHITECTURE & RELIABILITY ENGINEERING
------------------------------------------------------------------------------
[+] The Problem with Standard Watchdogs:
    In standard FreeRTOS implementations, if an application task hangs on a
    dead TCP socket, enters an infinite loop, or deadlocks on a mutex, the
    FreeRTOS scheduler falls back to the IDLE task. The IDLE task runs, feeds
    the hardware watchdog, and the microcontroller NEVER resets, remaining in
    a frozen "zombie" state indefinitely.

[+] The System Health Supervisor Architecture (v1.8.0):
    This project replaces isolated watchdog processes with a cooperative
    heartbeat matrix:
    1) Each critical task registers a heartbeat channel:
       - HEALTH_CHANNEL_SCHEDULER   (limit: 90s, runs every 30s)
       - HEALTH_CHANNEL_AUTOMATION  (limit: 90s, runs every 30s)
       - HEALTH_CHANNEL_WIFI        (limit: 45s, runs every 5s)
       - HEALTH_CHANNEL_STATE       (limit: 90s, runs every 30s)
       - HEALTH_CHANNEL_TELEGRAM    (limit: 90s, runs every 5s)
       - HEALTH_CHANNEL_AUTO_REBOOT (limit: 60s, runs every 5s)

    2) Hardware TWDT Coupling:
       The supervisor task (highest application priority) inspects all channels
       every 3 seconds. ONLY IF 100% of registered channels have reported a
       valid heartbeat within their deadlines does it call esp_task_wdt_reset_user().

    3) Guaranteed Recovery & Black-Box Logging:
       If any task misses its deadline:
       - The supervisor identifies the exact stalled task.
       - It records the failure, uptime, and last log lines into RTC Fast Memory
         (RTC_NOINIT_ATTR), which survives resets.
       - It withholds the TWDT feed and triggers esp_restart().
       - If software reset fails, the hardware silicon TWDT panics and resets
         the chip forcefully.
       - Upon rebooting, the Telegram bot automatically sends crash_log.txt
         with the pre-crash logs directly to the user.

[+] Clock & Scheduler Resilience:
    The scheduler previously missed wake events if SNTP took >15 minutes to
    sync after a reboot. In v1.8.0:
    - Confirmed UNIX time is periodically saved to RTC memory.
    - Upon reboot, time is restored immediately via settimeofday() from RTC
      before SNTP even connects.
    - The wake grace window is extended to 60 minutes (FIRE_GRACE_MIN = 60).


------------------------------------------------------------------------------
6. SETUP & FLASHING INSTRUCTIONS
------------------------------------------------------------------------------
[+] Prerequisites:
    - ESP-IDF v5.3 installed (https://docs.espressif.com/projects/esp-idf/).
    - Python 3.10+ with standard ESP-IDF virtualenv.

[+] Step 1: Clone the repository
    git clone https://github.com/your-username/esp32-server-remote-control.git
    cd esp32-server-remote-control

[+] Step 2: Configure Credentials
    Copy the example credentials file:
    cp sdkconfig.credentials.example sdkconfig.credentials

    Edit sdkconfig.credentials with your settings:
    -------------------------------------------------------------------
    # WiFi Settings
    CONFIG_WIFI_SSID="Your_WiFi_SSID"
    CONFIG_WIFI_PASSWORD="Your_WiFi_Password"

    # Tailscale Settings (https://login.tailscale.com/admin/settings/keys)
    CONFIG_TAILSCALE_AUTH_KEY="tskey-auth-kXXXXX-XXXXXXXXXXXXXX"
    CONFIG_TAILSCALE_DEVICE_NAME="esp32-server-ctrl"

    # Telegram Bot Settings (from @BotFather and @userinfobot)
    CONFIG_TELEGRAM_BOT_TOKEN="123456789:ABCdefGhIJKlmNoPQRsTUVwxyZ"
    CONFIG_TELEGRAM_AUTHORIZED_IDS="123456789,987654321"

    # Target Server Settings
    CONFIG_SERVER_IP="192.168.1.100"
    CONFIG_SERVER_HTTP_PORT=9999
    CONFIG_SERVER_SHUTDOWN_SECRET="Your_Generated_Secret_Token"
    CONFIG_SERVER_MAC_HEX="00:11:22:33:44:55"
    -------------------------------------------------------------------

[+] Step 3: Build the Firmware
    ./idf.sh build

[+] Step 4: Flash to ESP32
    Connect your ESP32-S3 via USB-C and run:
    ./idf.sh flash

    (Optional: View live boot logs in serial monitor)
    ./idf.sh monitor


------------------------------------------------------------------------------
7. SERVER-SIDE DAEMON SETUP (Python / systemd)
------------------------------------------------------------------------------
For the ESP32 to query server state, initiate S3 suspend, or power off, a
lightweight daemon runs on the target server.

[+] Create /usr/local/bin/esp32-shutdown-server.py:
-------------------------------------------------------------------
#!/usr/bin/env python3
"""ESP32 shutdown/suspend daemon (target server side).

IMPORTANT: the HTTP 200 is sent BEFORE the action runs, in a background
thread. If the action runs first, the machine suspends/powers off before
the response leaves the box and the gateway never sees the confirmation
(it then reports the command as failed and keeps stale state).
"""
import http.server
import json
import os
import secrets
import subprocess
import threading
import time

SECRET_FILE = "/etc/esp32-shutdown/secret"
PORT = 9999
# Bind to the LAN address of the server (not 0.0.0.0) to keep the daemon
# off the tailnet. The ESP32 reaches it over the LAN.
BIND = os.environ.get("ESP32_SHUTDOWN_BIND", "192.168.1.100")

def get_secret():
    with open(SECRET_FILE, "r") as f:
        return f.read().strip()

def run_action(action):
    cmd = ["sudo", "-n", "/usr/bin/systemctl", action]
    r = subprocess.run(cmd, timeout=20, capture_output=True, text=True)
    if r.returncode == 0:
        return True, action + " sent"
    # A shutdown inhibitor (unattended-upgrades, for instance) can block
    # the poweroff and leave the machine running: retry ignoring it.
    if action == "poweroff" and "inhibit" in (r.stderr or "").lower():
        r2 = subprocess.run(cmd + ["--force"], timeout=20,
                            capture_output=True, text=True)
        if r2.returncode == 0:
            return True, "poweroff sent (forced: shutdown inhibitor was active)"
    return False, action + " failed: " + (r.stderr or "").strip()

class ShutdownHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/state":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"state":"ON"}\n')
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        auth = self.headers.get("X-ESP32-Secret", "")
        if not secrets.compare_digest(auth, get_secret()):
            self.send_response(401)
            self.end_headers()
            return

        action = self.path.lstrip("/")
        if action in ("suspend", "poweroff", "hibernate"):
            # 1) answer first
            payload = json.dumps({"ok": True, "action": action}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            # 2) then act, in the background
            t = threading.Thread(target=self._run_later, args=(action,),
                                daemon=True)
            t.start()
        else:
            self.send_response(404)
            self.end_headers()

    def _run_later(self, action):
        time.sleep(0.4)   # let the 200 leave the box first
        ok, msg = run_action(action)
        print("%s: %s" % (action, msg), flush=True)

if __name__ == "__main__":
    server = http.server.ThreadingHTTPServer((BIND, PORT), ShutdownHandler)
    server.daemon_threads = True
    server.serve_forever()
-------------------------------------------------------------------

[+] Enable as a systemd service:
    sudo nano /etc/systemd/system/esp32-shutdown.service
    -------------------------------------------------------------------
    [Unit]
    Description=ESP32 Shutdown & State Daemon
    After=network-online.target
    Wants=network-online.target

    [Service]
    Type=simple
    ExecStart=/usr/bin/python3 /usr/local/bin/esp32-shutdown-server.py
    # on-failure, NOT always: during a poweroff systemd stops this service
    # and "always" would fight the shutdown transaction.
    Restart=on-failure
    RestartSec=5

    [Install]
    WantedBy=multi-user.target
    -------------------------------------------------------------------
    sudo systemctl daemon-reload
    sudo systemctl enable --now esp32-shutdown.service


------------------------------------------------------------------------------
8. PUBLIC RELEASE CHANGELOG
------------------------------------------------------------------------------
v1.8.3 - Power-off cycle (2026-10-20)
--------------------------------------------
- Single shared power-off sequence (WOL if the host is asleep, then /off),
  used by the Telegram /off, the inactivity automation and the daily
  schedule. Before this, the same logic was duplicated three times and each
  copy had the same defects:
  * an unreachable host was reported as "already off" without sending
    anything, so a host suspended in S3 stayed suspended forever (with its
    tailnet link down);
  * the S3 -> S0 -> S5 path blocked the calling task for up to ~90 s, more
    than the 90 s heartbeat budget of the health supervisor, so the gateway
    restarted itself in the middle of a shutdown and the bot went silent;
  * the shutdown is now confirmed by probing when the HTTP reply is lost
    (the daemon powers the machine off before it answers), and the Telegram
    command runs in the background so the bot keeps answering.
- S3 vs S5: the state machine no longer degrades a suspended host to OFF
  after 5 minutes, and the "the host was suspended" hint survives a
  gateway restart.
- The inactivity timer no longer cancels a pending power-off while the host
  is suspended (every portal hit used to cancel it).
- /diag now exposes the gateway health, uptime and last stall over HTTP.
- Server-side shutdown daemon (see section 7): it now answers BEFORE acting
  (the reply used to be lost while the machine suspended or powered off),
  retries the bind at boot, and forces the power-off when a shutdown
  inhibitor would otherwise keep the machine running.

v1.8.4 - Power-cut detection and automatic recovery (2026-10-20)
--------------------------------------------
- The gateway watches its OWN connectivity: when the network drops for more than
  20 s and comes back while the server was supposed to be running, it treats
  it as a power cut (the gateway and the server share the same installation).
- On a detected cut it brings the server back on its own: the wake supervisor is
  re-armed every 2 minutes (each episode bursts magic packets every 10 s for 4
  minutes) instead of a single burst that can be lost while the router and the
  NIC are still settling.
- If the server does not come back after 15 attempts (~30 min), the gateway
  gives up and says so over Telegram, with the three BIOS settings that make a
  machine boot by itself when the mains returns (AC BACK = Always On, ErP =
  Disabled, Power On By PME/PCI-E = Enabled).
- A server that was switched off on purpose is never woken up: the gateway
  forgets about it as soon as it has given up.
- POST /diag/outage (private port only) simulates a network outage so the whole
  path can be tested without cutting the power.
- /diag now also reports the gateway health, its uptime, the last stall and the
  state of the power-cut watcher.

v1.8.2 - Evidence-based readiness gate (2026-10-15)
--------------------------------------------
- The gateway never waits a fixed amount of time before forwarding a client
  to the server: a background task probes the real destination and publishes
  a latch as soon as it answers (1 s cadence while somebody waits).
- The server can announce its own readiness to the gateway
  (GET /notify/ready, and /notify/unready when the public path stops
  serving). A systemd unit is included under tools/.
- New /schedule endpoint for the daily wake/off program.

v1.8.1 - Auto-WOL resilience (2026-10-14)
--------------------------------------------
- Firmware-side wake retries, verified redirection and a cold-boot gate.
- Decoupled the blocking sockets from the single-threaded web server.
- Protection against a reload storm from the waiting splash page.

v1.8.0 - Initial Public Release (2026-09-13)
-------------------------------------------
- System Health Supervisor:
  * Multi-task cooperative heartbeat matrix (Scheduler, Automation, WiFi,
    State Poll, Telegram, Auto-reboot).
  * Direct coupling with ESP-IDF Hardware Task Watchdog Timer (TWDT).
  * Emergency hardware-assisted restart upon task stall detection.

- Black-Box RTC Memory & Telegram Log File Export:
  * Intercepts all ESP_LOG* calls into a 64 KB PSRAM circular buffer.
  * Preserves the last 3 KB of logs prior to a crash in RTC Fast Memory.
  * Native multipart/form-data sendDocument implementation in Telegram client.
  * New /logs command: exports live log buffer as esp32_log.txt.
  * New /crashlog command: exports RTC pre-crash buffer as crash_log.txt.
  * Automatic Telegram dispatch of crash_log.txt upon watchdog recovery.

- Clock & Scheduler Hardening:
  * Wall-clock persistence across reboots via RTC Fast Memory.
  * Immediate settimeofday() restoration on boot before SNTP sync.
  * Extended wake grace window to 60 minutes.
  * Immediate SNTP trigger upon WiFi IP acquisition.

- Multi-Destination Wake-on-LAN:
  * 4-way broadcast and unicast burst with static ARP injection.

- Tailscale MicroLink v2:
  * Native WireGuard integration on ESP-IDF v5.3.
  * Automatic STUN and DERP relay fallback.

==============================================================================
End of README.txt
------------------------------------------------------------------------------
9. ACKNOWLEDGMENTS & CREDITS
------------------------------------------------------------------------------
This project was built upon the pioneering work of open-source developers who
first made WireGuard and Tailscale possible on microcontrollers:

- Samir Makwana (@samirmakwana):
  Creator of ESP32-Tailscale-WoL (https://github.com/samirmakwana/ESP32-Tailscale-WoL).
  This repository provided the initial project scaffold and inspiration when
  migrating the firmware from Arduino to ESP-IDF, demonstrating the baseline
  concept of triggering Wake-on-LAN over a Tailscale connection on ESP32.

- Cameron Malone (@CamM2325):
  Creator of MicroLink (https://github.com/CamM2325/microlink), one of the
  very few implementations of the Tailscale coordination protocol (ts2021)
  for the ESP32. This project integrates MicroLink v2 as its VPN foundation
  and built extensive resilience layers around it (resolving silent TCP half-open
  session drops, zombie reconnection loops, and multi-interface failovers).

- Daniel Hope (@danielhope / Floorsense Ltd):
  Creator of wireguard_lwip (https://www.floorsense.nz), providing the
  malloc-free C implementation of the WireGuard protocol for the lwIP stack.

- Jason A. Donenfeld:
  Creator of the WireGuard protocol (https://www.wireguard.com).

- Daniel J. Bernstein:
  Creator of the X25519 elliptic curve implementation used for key exchange.


------------------------------------------------------------------------------
10. LICENSE
------------------------------------------------------------------------------
This project is open-source software licensed under the MIT License.
See the LICENSE file for full license terms and third-party copyright notices.
You are free to use, modify, distribute, and integrate this software in
personal or commercial projects without restriction.

==============================================================================