#!/usr/bin/env python3
"""
labelsis-ota.py -- upload a firmware image to a LabelSis device over HTTP.

Mirrors what the SPA's "Firmware update" panel does (POST /api/ota), but
from a terminal -- handy for headless flashing, scripted rollouts, or
when the SPA can't reach the device because of a browser quirk (see
pt700-cbb HAR analysis: socket starvation under aggressive polling).

Stdlib-only (no `requests` dep).

Usage:
    labelsis-ota.py --host labelsis.local \\
                    --file release/labelsis-devkitc_s3-v0.8.0.bin

Exit codes:
    0 -- upload OK, device reboot acknowledged
    1 -- argument / file error
    2 -- OTA gate closed (slide printer to P-Lite, or hold BOOT 2-30 s
         and release while device is online)
    3 -- HTTP error from /api/ota (image_validate, wrong_project, ...)
    4 -- network / socket error during upload
"""

import argparse
import http.client
import json
import socket
import sys
import time
from pathlib import Path


def get_ota_status(conn: http.client.HTTPConnection) -> dict:
    conn.request("GET", "/api/ota/status")
    r = conn.getresponse()
    body = r.read()
    if r.status != 200:
        raise RuntimeError(f"GET /api/ota/status -> HTTP {r.status}: {body!r}")
    return json.loads(body)


def upload(host: str, port: int, path: Path, timeout: float, chunk: int) -> int:
    size = path.stat().st_size
    print(f"target: http://{host}:{port}/api/ota")
    print(f"image:  {path.name} ({size:,} bytes)")

    # Pre-flight: confirm gate is open. Saves the user a 1.5 MB upload that
    # would just be rejected with 403 gate_closed.
    pre = _conn(host, port, timeout=10)
    try:
        st = get_ota_status(pre)
    except (socket.error, RuntimeError) as e:
        print(f"error: cannot reach device: {e}", file=sys.stderr)
        return 4
    finally:
        pre.close()
    if not st.get("available"):
        print(f"error: OTA gate closed: {st.get('reason', '?')}", file=sys.stderr)
        print("  - slide the printer's switch to EL (P-Lite mode), OR", file=sys.stderr)
        print("  - hold the BOOT button 2-30 s and release", file=sys.stderr)
        return 2
    print(f"gate:   open ({st.get('reason', '?')})")
    running = st.get("app", {}).get("version", "?")
    print(f"device: running {running}, will write {st.get('next_slot', '?')}")
    print()

    # The actual upload. http.client lets us stream the file in chunks
    # while keeping a single Content-Length-bounded request -- which is
    # what the device expects (no chunked transfer-encoding).
    try:
        c = _conn(host, port, timeout=timeout)
        c.putrequest("POST", "/api/ota")
        c.putheader("Content-Type",   "application/octet-stream")
        c.putheader("Content-Length", str(size))
        c.putheader("Connection",     "close")
        c.endheaders()

        sent     = 0
        last_pct = -1
        started  = time.monotonic()
        with path.open("rb") as fh:
            while True:
                buf = fh.read(chunk)
                if not buf:
                    break
                c.send(buf)
                sent += len(buf)
                pct = sent * 100 // size
                if pct != last_pct:
                    elapsed = time.monotonic() - started
                    rate    = sent / max(elapsed, 1e-3) / 1024
                    print(f"\r  {pct:3d}%  {sent:>9,} / {size:,}  {rate:>5.0f} KiB/s",
                          end="", flush=True)
                    last_pct = pct
        print()  # newline after progress

        # The firmware now (a) runs esp_ota_end's SHA verify -- a few seconds
        # on real flash for a 1.5 MB image, (b) sends the 200 response, then
        # (c) sleeps 2 s and esp_restart()s. Show that we're waiting for
        # the verify+respond step instead of looking hung. Tighter timeout
        # here than the upload phase: if the response doesn't land in 30 s,
        # the device most likely rebooted before the FIN flushed -- treat
        # as "probably succeeded" and check via status poll.
        print("waiting for verify + response (≤30 s)…", flush=True)
        c.sock.settimeout(30.0)
        r    = c.getresponse()
        body = r.read().decode("utf-8", "replace")
    except (socket.timeout, TimeoutError):
        # Likely: response was sent but the reboot interrupted the FIN.
        # Confirm by polling /api/status -- if the device comes back with
        # the new image's version, we're done.
        print()
        print("no response within 30 s -- device may have rebooted before FIN.")
        print("polling /api/status to confirm…")
        try: c.close()
        except Exception: pass
        return _wait_for_reboot(host, port)
    except (socket.error, http.client.HTTPException) as e:
        print(f"\nerror: network failure during upload: {e}", file=sys.stderr)
        try: c.close()
        except Exception: pass
        return 4
    finally:
        try: c.close()
        except Exception: pass

    if r.status >= 200 and r.status < 300:
        try:
            j = json.loads(body)
        except json.JSONDecodeError:
            print(f"warning: 200 OK but body wasn't JSON: {body!r}", file=sys.stderr)
            return 0
        ms = j.get("reboot_in_ms", 2000)
        print(f"installed v{j.get('version', '?')} into {j.get('slot', '?')}")
        print(f"device rebooting in {ms} ms; rollback watchdog cancels on next "
              f"successful boot.")
        # Wait briefly past the firmware's own reboot-in-ms grace, then
        # poll /api/status until the device is responsive again. The SPA
        # does the same thing -- gives the user a clear "back online"
        # signal instead of leaving them to guess.
        time.sleep(ms / 1000.0 + 1.5)
        return _wait_for_reboot(host, port)

    # Best-effort error decode -- the firmware always returns
    # {"ok":false,"error":"<token>"} on failure paths.
    try:
        j   = json.loads(body)
        err = j.get("error", body)
    except json.JSONDecodeError:
        err = body
    print(f"error: HTTP {r.status} {r.reason}: {err}", file=sys.stderr)
    return 3


def _conn(host: str, port: int, timeout: float) -> http.client.HTTPConnection:
    c = http.client.HTTPConnection(host, port, timeout=timeout)
    c.connect()
    return c


def _wait_for_reboot(host: str, port: int, deadline_s: float = 90.0) -> int:
    """Poll /api/status until the device answers (it just rebooted into
    the new image) or until deadline. Returns 0 on success, 4 on
    timeout. Mirrors what the SPA does in otaWaitForReboot."""
    started = time.monotonic()
    print("  ", end="", flush=True)
    while time.monotonic() - started < deadline_s:
        try:
            c = _conn(host, port, timeout=3.0)
            try:
                c.request("GET", "/api/status")
                r = c.getresponse()
                _ = r.read()
                # Both 200 (printer attached) and 503 (no_printer) mean
                # the device is back -- we just want a TCP+HTTP signal.
                if r.status in (200, 503):
                    elapsed = time.monotonic() - started
                    print(f" up after {elapsed:.0f} s", flush=True)
                    # Try to surface the new running version for confirmation.
                    try:
                        c2 = _conn(host, port, timeout=5.0)
                        c2.request("GET", "/api/ota/status")
                        r2 = c2.getresponse()
                        st = json.loads(r2.read())
                        v  = st.get("app", {}).get("version", "?")
                        print(f"running version: {v}")
                        c2.close()
                    except Exception:
                        pass
                    return 0
            finally:
                c.close()
        except (socket.error, http.client.HTTPException, json.JSONDecodeError):
            pass
        print(".", end="", flush=True)
        time.sleep(2.0)
    print(f" timeout after {deadline_s:.0f} s", flush=True)
    print("warning: device did not come back. Check serial / power-cycle.",
          file=sys.stderr)
    return 4


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Upload a firmware image to a LabelSis device.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage:", 1)[1].lstrip() if "Usage:" in __doc__ else "",
    )
    ap.add_argument("--host", required=True,
                    help="device hostname or IP, e.g. labelsis.local or 192.168.1.42")
    ap.add_argument("--port", type=int, default=80, help="HTTP port (default: 80)")
    ap.add_argument("--file", required=True, type=Path,
                    help="path to labelsis-<board>-<version>.bin (NOT -merged.bin)")
    ap.add_argument("--timeout", type=float, default=180.0,
                    help="per-request socket timeout, seconds (default: 180)")
    ap.add_argument("--chunk", type=int, default=16 * 1024,
                    help="upload chunk size in bytes (default: 16384)")
    args = ap.parse_args(argv)

    if not args.file.exists():
        print(f"error: file not found: {args.file}", file=sys.stderr)
        return 1
    if not args.file.is_file():
        print(f"error: not a regular file: {args.file}", file=sys.stderr)
        return 1
    if args.file.name.endswith("-merged.bin"):
        print(f"error: {args.file.name} is the merged-flash bundle, not the OTA image.\n"
              f"       use the file ending in just .bin (no -merged suffix).",
              file=sys.stderr)
        return 1

    return upload(args.host, args.port, args.file, args.timeout, args.chunk)


if __name__ == "__main__":
    sys.exit(main())
