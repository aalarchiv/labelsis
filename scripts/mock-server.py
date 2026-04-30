#!/usr/bin/env python3
"""
Single-file mock of the pt700 firmware HTTP API.

Serves the SPA from disk and replies to /api/* with sensible fake
values so the web UI can be developed without flashing the ESP or
even owning the device.

Usage:
    python3 scripts/mock-server.py [--port 8080] [--bind 127.0.0.1]

Then visit http://localhost:8080/ -- same-origin, so no API host
config is needed in the SPA's Settings tab.

Edit STATE / TAPES / SCAN below to simulate different printer
conditions (cover open, no media, different tape size, etc.).
"""

import argparse
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

SPA_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "components", "pt_app", "spa",
))

# Mutable state -- adjust to simulate different printer conditions.
# `transport` controls what the SPA's title-bar dot shows: "usb_host"
# is green ("real" printer attached), "mock" / "plite" are red.
STATE = {
    "transport":      "usb_host",
    "model":          0x70,        # PT-P700
    "media_width_mm": 12,
    "media_type":     0x01,
    "tape_color_id":  0x01,
    "text_color_id":  0x08,
    "error1":         0,
    "error2":         0,
    "status_type":    0,           # idle
    "phase_type":     0,

    # USB identity surfaced via /api/info so the SPA Status view can
    # show what's plugged in. These mirror what the firmware will
    # eventually report from real string descriptors (pt700-... bd
    # issue) -- for now mock values matching a real PT-P700 in
    # printer mode (E slider position).
    "vid":            0x04F9,      # Brother
    "pid":            0x2061,      # PT-P700 (E mode)
    "serial":         "MOCK-0000001",
    "manufacturer":   "Brother (mock)",
    "product":        "Brother PT-P700 (mock)",
    "fw_version":     "mock-1.0",
}

# Tape geometry table from components/pt_protocol/src/geometry.c.
# Keep in sync if that table changes.
TAPES = {
    4:  {"left": 52, "print":  24, "right": 52, "tape_dots":  24},
    6:  {"left": 48, "print":  32, "right": 48, "tape_dots":  42},
    9:  {"left": 39, "print":  50, "right": 39, "tape_dots":  64},
    12: {"left": 29, "print":  70, "right": 29, "tape_dots":  84},
    18: {"left":  8, "print": 112, "right":  8, "tape_dots": 128},
    24: {"left":  0, "print": 128, "right":  0, "tape_dots": 170},
}

# Fake nearby Wi-Fi networks for /api/scan.
SCAN_NETWORKS = [
    {"ssid": "Home Wi-Fi",   "rssi": -45, "auth": 3},
    {"ssid": "Guest",        "rssi": -62, "auth": 0},
    {"ssid": "neighbour-3F", "rssi": -78, "auth": 3},
]

CORS = {
    "Access-Control-Allow-Origin":  "*",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "*",
    "Access-Control-Max-Age":       "3600",
}

# Headers the firmware accepts on /api/print -- log them for fidelity.
PRINT_HEADERS = (
    "X-Auto-Cut", "X-Mirror", "X-Chain", "X-No-Compression",
    "X-Margin-Dots", "X-Tape-Width-Mm",
)


class Handler(BaseHTTPRequestHandler):

    # ---- helpers ----------------------------------------------------

    def _send_json(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        for k, v in CORS.items():
            self.send_header(k, v)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_file(self, path, content_type):
        try:
            with open(path, "rb") as f:
                body = f.read()
        except FileNotFoundError:
            self.send_error(404, "not found")
            return
        self.send_response(200)
        for k, v in CORS.items():
            self.send_header(k, v)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        # One-line per request, with our timestamp.
        sys.stderr.write(f"[{time.strftime('%H:%M:%S')}] "
                         f"{self.address_string()} {fmt % args}\n")

    def log_extra(self, line):
        sys.stderr.write(f"           {line}\n")

    # ---- HTTP methods -----------------------------------------------

    def do_OPTIONS(self):
        self.send_response(204)
        for k, v in CORS.items():
            self.send_header(k, v)
        self.end_headers()

    def do_GET(self):
        path = urlparse(self.path).path

        if path in ("/", "/index.html"):
            self._send_file(os.path.join(SPA_DIR, "index.html"), "text/html")
            return

        if path == "/qrcode.min.js":
            self._send_file(os.path.join(SPA_DIR, "qrcode.min.js"),
                            "application/javascript")
            return

        if path == "/setup.html":
            self._send_file(os.path.join(SPA_DIR, "setup.html"), "text/html")
            return

        if path.startswith("/i18n/") and path.endswith(".json"):
            # Whitelist filename to a single segment so the path can't
            # walk out of SPA_DIR. The SPA only ever asks for files
            # like i18n/de.json, never anything fancier.
            name = os.path.basename(path)
            if "/" in name or "\\" in name or name.startswith("."):
                self.send_error(404, "not found")
                return
            self._send_file(os.path.join(SPA_DIR, "i18n", name), "application/json")
            return

        if path == "/fonts/bootstrap-icons.woff2":
            self._send_file(os.path.join(SPA_DIR, "fonts", "bootstrap-icons.woff2"),
                            "font/woff2")
            return

        if path == "/fonts/bootstrap-icons.json":
            self._send_file(os.path.join(SPA_DIR, "fonts", "bootstrap-icons.json"),
                            "application/json")
            return

        if path == "/api/status":
            self._send_json(200, {**STATE, "ok": True})
            return

        if path == "/api/info":
            tape = TAPES.get(STATE["media_width_mm"], TAPES[12])
            offside = (tape["tape_dots"] - tape["print"]) // 2
            self._send_json(200, {
                "ok":             True,
                "transport":      STATE["transport"],
                "model":          STATE["model"],
                "media_width_mm": STATE["media_width_mm"],
                "media_type":     STATE["media_type"],
                "tape_color_id":  STATE["tape_color_id"],
                "text_color_id":  STATE["text_color_id"],
                "vid":            STATE["vid"],
                "pid":            STATE["pid"],
                "serial":         STATE["serial"],
                "manufacturer":   STATE["manufacturer"],
                "product":        STATE["product"],
                "fw_version":     STATE["fw_version"],
                "geometry": {
                    "head_pins":                   128,
                    "print_pins":                  tape["print"],
                    "tape_width_dots":             tape["tape_dots"],
                    "left_margin_pins":            tape["left"],
                    "right_margin_pins":           tape["right"],
                    "non_printable_dots_per_side": offside,
                },
            })
            return

        if path == "/api/scan":
            time.sleep(0.5)        # pretend scanning takes a moment
            self._send_json(200, {"ok": True, "networks": SCAN_NETWORKS})
            return

        self.send_error(404, "not found")

    def do_POST(self):
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", "0") or 0)
        body = self.rfile.read(length) if length > 0 else b""

        if path == "/api/print":
            if length == 0:
                self._send_json(400, {"ok": False, "error": "missing_body"})
                return
            if length % 16 != 0:
                self._send_json(400, {"ok": False, "error": "length_not_aligned_16"})
                return
            rows = length // 16
            for h in PRINT_HEADERS:
                if h in self.headers:
                    self.log_extra(f"{h}: {self.headers[h]}")
            self.log_extra(f"body: {length} bytes ({rows} rows)")
            # Fake the printer's actual print time so the SPA gets a
            # realistic feel for "session locked, status temporarily
            # busy". 5 ms per row plus 2 s warm-up + cut.
            time.sleep(min(2.0 + rows * 0.005, 8.0))
            self._send_json(200, {"ok": True, "rows": rows})
            return

        if path == "/api/cut":
            time.sleep(0.5)
            self._send_json(200, {"ok": True})
            return

        if path == "/api/feed":
            time.sleep(0.5)
            self._send_json(200, {"ok": True})
            return

        if path == "/api/setup":
            try:
                data = json.loads(body) if body else {}
            except json.JSONDecodeError:
                self._send_json(400, {"ok": False, "error": "bad_json"})
                return
            self.log_extra(f"ssid={data.get('ssid')!r} (real device would reboot)")
            self._send_json(200, {"ok": True})
            return

        self.send_error(404, "not found")


def main():
    p = argparse.ArgumentParser(description="pt700 mock HTTP server")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--bind", default="127.0.0.1",
                   help="interface to bind (use 0.0.0.0 for LAN access)")
    args = p.parse_args()

    if not os.path.isdir(SPA_DIR):
        sys.stderr.write(f"mock-server: SPA dir not found: {SPA_DIR}\n")
        return 1

    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    print(f"pt700-mock listening on http://{args.bind}:{args.port}/")
    print(f"  serving SPA from {SPA_DIR}")
    print(f"  edit STATE / TAPES at top of {__file__} to simulate other states")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nshutting down")
    return 0


if __name__ == "__main__":
    sys.exit(main())
