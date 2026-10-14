#!/usr/bin/env python3
"""WatchTower mock device.

A standard-library HTTP and WebSocket server that stands in for the firmware
(components/wt_app_web/wt_app_web.c), so the web console in web/
can be worked on in a desktop browser without a board.

It serves web/ unchanged and sends the firmware's WebSocket frames
with sample data:
  - "hello" and "info" once per connection
  - "disp"  every 50 ms (display state)
  - "full"  every 2 s (system stats; wifi, power and settings only in the
            first one, as the firmware sends null for unchanged parts)
  - "scan"  once, with sample networks

Usage:
    python3 simulation/mock_device/mock_server.py [--port 8080] [--host 127.0.0.1]

Then open http://<host>:<port>/. Edit the *_frame() functions and INFO to
try other states.

GET /preset?tab=network&theme=light opens the console on a given tab and
theme.
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import struct
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "web"))
WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WS_TICK_S = 0.05          # matches WS_DISPLAY_INTERVAL_MS in wt_app_web.c
WS_FULL_EVERY_N_TICKS = 40  # 40 * 50ms = 2s, matches WS_FULL_INTERVAL_MS


def ws_accept_key(key):
    return base64.b64encode(hashlib.sha1((key + WS_MAGIC).encode()).digest()).decode()


def send_ws_text(conn, text):
    payload = text.encode()
    n = len(payload)
    if n < 126:
        header = bytes([0x81, n])
    elif n < 65536:
        header = bytes([0x81, 126]) + struct.pack(">H", n)
    else:
        header = bytes([0x81, 127]) + struct.pack(">Q", n)
    conn.sendall(header + payload)


# Sample "info" frame, sent once per connection.
INFO = {
    "type": "info",
    "chip_model": "ESP32-S3",
    "cpu_cores": 2,
    "cpu_freq_mhz": 240,
    "flash_size": 8388608,
    "app_version": "1.0.0",
    "build_date": "Jul 10 2026 09:00:00",
    "idf_version": "v5.2.1",
    "ota_slot": "app0",
    "app0_state": "valid",
    "app1_state": "empty",
    "min_leds": 58,
    "max_leds": 100,
    "reset_reason": "power-on",
}

# Segment masks for a static "12:34" preview (see MASK_TO_CHAR in app.js)
DIGITS_12_34 = [63, 6, 79, 102]


def full_frame(tick):
    """Sample periodic "full" frame. wifi/power/settings are only populated
    on tick 0 (first full frame after connect) everywhere else they are
    None (-> JSON null), mirroring the firmware's generation-counter gating
    that skips re-sending sub-objects that haven't changed."""
    return {
        "type": "full",
        "cpu_usage": 12,
        "flash_used_pct": 34,
        "free_heap": 186000,
        "total_heap": 320000,
        "min_free_heap": 150000,
        "uptime_s": 8134 + tick,
        "temperature": 46.2,
        "sta_connected": True,
        "sta_ssid": "HomeNet-5G",
        "sta_ip": "192.168.1.42",
        "rssi": -54,
        "ap_ip": "192.168.4.1",
        "display": {
            "available": True,
            "mode": "time",
            "brightness": 80,
            "colon": (tick % 2 == 0),
            "colon_blink": True,
            "time_format": 24,
            "digits": DIGITS_12_34,
            "color_on": [0, 200, 255],
            "color_off": [8, 8, 8],
            "colon_color": [0, 200, 255],
        },
        "wifi": {
            "connected": True,
            "ssid": "HomeNet-5G",
            "ip": "192.168.1.42",
            "rssi": -54,
            "channel": 6,
            "ap_ip": "192.168.4.1",
            "ap_active": True,
            "ap_ssid": "WatchTower",
            "ap_clients": 0,
            "hostname": "watchtower",
            "profiles": [
                {"idx": 0, "ssid": "HomeNet-5G", "has_pass": True},
                {"idx": 1, "ssid": "Office-Guest", "has_pass": True},
            ],
        } if tick == 0 else None,
        "power": {
            "battery_pct": 76,
            "voltage": 3.98,
            "current": 210,
            "source": "USB",
            "eta_hours": 5.5,
            "sleep_mode": "none",
            "sleep_timeout": 30,
            "batt_alert_pct": 20,
            "ps_dim": True,
            "ps_wifi": False,
        } if tick == 0 else None,
        "settings": {
            "color": "#00c8ff",
            "brightness": 80,
            "anim_colon": True,
            "anim_scroll": False,
            "anim_transition": True,
            "display_mode": "time",
            "display_value": 1234,
            "display_text": "HELO",
            "bg_effect_en": False,
            "anim_effect": "rainbow_ring",
            "led_count": 58,
            "time_format": 24,
            "timezone": "UTC0",
            "ntp_server": "pool.ntp.org",
            "alarm1_time": "07:00",
            "alarm1_en": False,
            "alarm2_time": "22:00",
            "alarm2_en": False,
            "notif_type": "flash",
            "notif_sound": "beep",
            "sleep_mode": "none",
            "sleep_timeout": 30,
            "batt_alert_pct": 20,
            "ps_dim": True,
            "ps_wifi": False,
        } if tick == 0 else None,
        "logs": {
            "seq": tick,
            "entries": ["[I] wifi: connected to HomeNet-5G", "[I] ntp: synced"] if tick == 0 else [],
        },
    }


def disp_frame(tick):
    """Sample "disp" (display-only) frame."""
    return {
        "type": "disp",
        "display": {
            "available": True,
            "mode": "time",
            "brightness": 80,
            "colon": (tick % 10 < 5),
            "colon_blink": True,
            "time_format": 24,
            "digits": DIGITS_12_34,
            "color_on": [0, 200, 255],
            "color_off": [8, 8, 8],
            "colon_color": [0, 200, 255],
        },
    }


# Sample "scan" frame. The mock does not read commands, so it is sent once
# after connecting.
SCAN = {
    "type": "scan",
    "scanning": False,
    "networks": [
        {"ssid": "HomeNet-5G", "rssi": -54, "channel": 6, "secure": True},
        {"ssid": "Office-Guest", "rssi": -63, "channel": 1, "secure": True},
        {"ssid": "Cafe Free WiFi With A Rather Long Name", "rssi": -71, "channel": 11, "secure": False},
        {"ssid": "Neighbour_2.4G", "rssi": -82, "channel": 3, "secure": True},
    ],
}


def ws_client_loop(conn):
    send_ws_text(conn, json.dumps({
        "type": "hello", "proto": 1,
        "features": ["display", "wifi", "power", "settings", "logs"],
    }))
    send_ws_text(conn, json.dumps(INFO))
    send_ws_text(conn, json.dumps(SCAN))
    tick = 0
    try:
        while True:
            if tick % WS_FULL_EVERY_N_TICKS == 0:
                send_ws_text(conn, json.dumps(full_frame(tick // WS_FULL_EVERY_N_TICKS)))
            else:
                send_ws_text(conn, json.dumps(disp_frame(tick)))
            tick += 1
            time.sleep(WS_TICK_S)
    except OSError:
        pass  # client disconnected


def make_handler(root):
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=root, **kwargs)

        def do_GET(self):
            if self.path.startswith("/preset"):
                self._serve_preset()
                return
            if self.path == "/ws":
                self._upgrade_ws()
                return
            super().do_GET()

        def _serve_preset(self):
            import urllib.parse
            tab, theme = "display", "dark"
            if "?" in self.path:
                q = urllib.parse.parse_qs(self.path.split("?", 1)[1])
                tab = q.get("tab", [tab])[0]
                theme = q.get("theme", [theme])[0]
            body = (
                f"<script>localStorage.setItem('wt-theme','{theme}');"
                f"location.replace('/index.html#{tab}');</script>"
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _upgrade_ws(self):
            key = self.headers.get("Sec-WebSocket-Key")
            if not key:
                self.send_response(400)
                self.end_headers()
                return
            accept = ws_accept_key(key)
            resp = (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
            )
            self.connection.sendall(resp.encode())
            ws_client_loop(self.connection)

        def log_message(self, fmt, *args):
            pass  # keep the console quiet; this is a dev tool, not a service

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    if not os.path.isfile(os.path.join(ROOT, "index.html")):
        raise SystemExit(f"web/index.html not found under {ROOT} run from a checkout of the repo")

    srv = http.server.ThreadingHTTPServer((args.host, args.port), make_handler(ROOT))
    print(f"WATCH TOWER mock device serving {ROOT}")
    print(f"  → http://{args.host}:{args.port}/")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
