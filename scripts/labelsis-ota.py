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
    0 -- upload OK, device rebooted into the new image (running_slot
         flipped, confirmed via post-reboot poll)
    1 -- argument / file error
    2 -- OTA gate closed (slide printer to P-Lite, or hold BOOT 2-30 s
         and release while device is online)
    3 -- HTTP error from /api/ota (image_validate, wrong_project, ...)
    4 -- network / socket error during upload, or device didn't come
         back in time
    5 -- device came back but running_slot didn't flip -- the upload
         was rejected (check serial logs)
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
    running       = st.get("app", {}).get("version", "?")
    pre_run_slot  = st.get("running_slot", "?")
    pre_next_slot = st.get("next_slot", "?")
    print(f"device: running {running} from {pre_run_slot}, will write {pre_next_slot}")
    print()

    # Raw socket upload. We bypass http.client here for two reasons:
    #
    # 1. Real-hardware reproduction: with the http.client.request(...,
    #    body=fh) pattern, on a slow Wi-Fi link the response read hung
    #    at the socket timeout while the very same payload uploaded via
    #    curl in 21 s and returned 200 OK with the install JSON. Raw
    #    socket I/O matches what curl does -- byte for byte -- and
    #    sidesteps any state-machine quirk in the stdlib client.
    #
    # 2. Lets us send the request line + headers ourselves, which makes
    #    failures like "no response after upload" easier to reason
    #    about: the wire is right there in the loop.
    started = time.monotonic()
    def _print_progress(sent: int) -> None:
        pct = sent * 100 // size
        elapsed = time.monotonic() - started
        rate    = sent / max(elapsed, 1e-3) / 1024
        print(f"\r  {pct:3d}%  {sent:>9,} / {size:,}  {rate:>5.0f} KiB/s",
              end="", flush=True)

    try:
        s = socket.create_connection((host, port), timeout=timeout)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError as e:
        print(f"error: cannot connect: {e}", file=sys.stderr)
        return 4

    try:
        # Match curl's request-line + minimal headers. `Connection: close`
        # is intentional: the firmware's response is the last thing on
        # this socket, and we want a clean FIN as the EOF signal so the
        # response reader doesn't have to know about Content-Length.
        req = (
            f"POST /api/ota HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            f"User-Agent: labelsis-ota.py/1.0\r\n"
            f"Accept: */*\r\n"
            f"Content-Type: application/octet-stream\r\n"
            f"Content-Length: {size}\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        ).encode("ascii")
        s.sendall(req)

        sent = 0
        last_reported = -1
        sent_truncated = False
        with path.open("rb") as fh:
            while True:
                buf = fh.read(chunk)
                if not buf:
                    break
                try:
                    s.sendall(buf)
                except (BrokenPipeError, ConnectionResetError) as e:
                    # The device closed the connection mid-upload.
                    # Common cause: api_ota_upload returns early on
                    # esp_ota_write failure (e.g. flash slot in a bad
                    # state from a prior aborted OTA) and closes the
                    # socket while we're still streaming. Its 500
                    # response is in the TCP buffer but we may or may
                    # not be able to read it -- try.
                    print(f"\ndevice closed connection at {sent:,} / "
                          f"{size:,} bytes ({e})", file=sys.stderr)
                    sent_truncated = True
                    break
                sent += len(buf)
                pct = sent * 100 // size
                if pct != last_reported:
                    _print_progress(sent)
                    last_reported = pct
        if not sent_truncated:
            print()  # newline after progress

        # NOTE: NO socket.shutdown(SHUT_WR) here. Tried it; tcpdump
        # showed the FIN goes to the device, the device ACKs it, and
        # then never sends a response. esp_http_server seems to abort
        # the in-flight handler when the client half-closes -- even
        # though Content-Length was satisfied. curl never half-closes
        # and gets a clean response, so neither do we.

        # Drain the response. Connection: close means the server
        # FINs after the body, which gives us a clean EOF: keep
        # recv()ing until we get b''. The response is small (one
        # JSON line) so this is a few KB at most.
        print("waiting for verify + response…", flush=True)
        chunks = []
        while True:
            try:
                buf = s.recv(4096)
            except (socket.timeout, TimeoutError):
                # No FIN yet -- the typical case after a successful
                # OTA: the firmware sends the response then esp_restart()s
                # ~2 s later, and esp_restart's hardware reset can yank
                # the radio before the FIN flushes. We can't tell from
                # here whether the upload was accepted or rejected --
                # poll for boot completion and check whether running_slot
                # flipped (the unambiguous "image installed" signal).
                print("no response (typical -- reboot interrupted the FIN). "
                      "Polling /api/status to check whether the image took…")
                return _wait_for_reboot(host, port,
                                        expect_slot_flip=pre_run_slot)
            if not buf:
                break
            chunks.append(buf)
        raw = b"".join(chunks)
    except OSError as e:
        print(f"\nerror: network failure during upload: {e}", file=sys.stderr)
        return 4
    finally:
        try: s.close()
        except Exception: pass

    # Split status-line + headers + body. HTTP/1.1 framing.
    head, _, body_bytes = raw.partition(b"\r\n\r\n")
    first = head.split(b"\r\n", 1)[0]
    try:
        proto, status_str, *reason_parts = first.split(b" ", 2)
        status = int(status_str)
        reason = (reason_parts[0] if reason_parts else b"").decode(
            "iso-8859-1", "replace")
    except (ValueError, IndexError):
        print(f"error: malformed response: {raw[:200]!r}", file=sys.stderr)
        return 4
    body = body_bytes.decode("utf-8", "replace")
    # Synthesise a getresponse-shaped object so the success-handling
    # code below stays unchanged.
    class _R: pass
    r = _R()
    r.status = status
    r.reason = reason

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
        return _wait_for_reboot(host, port, expect_slot_flip=pre_run_slot)

    # Best-effort error decode -- the firmware returns
    # {"ok":false,"error":"<token>","esp_err":"ESP_ERR_..."} on
    # failure paths (esp_err only present when there's an underlying
    # ESP-IDF error worth surfacing -- ota_write, ota_begin, etc.).
    try:
        j   = json.loads(body)
        err = j.get("error", body)
        ctx = f" (esp_err: {j['esp_err']})" if "esp_err" in j else ""
    except json.JSONDecodeError:
        err = body
        ctx = ""
    print(f"error: HTTP {r.status} {r.reason}: {err}{ctx}", file=sys.stderr)
    return 3


def _conn(host: str, port: int, timeout: float) -> http.client.HTTPConnection:
    c = http.client.HTTPConnection(host, port, timeout=timeout)
    c.connect()
    return c


def _wait_for_reboot(host: str, port: int, deadline_s: float = 90.0,
                     expect_slot_flip: str = None) -> int:
    """Poll /api/status until the device answers, then check whether
    the OTA actually installed by looking at /api/ota/status's
    running_slot. If expect_slot_flip is given, success requires the
    slot to differ from that value -- the version string alone is
    unreliable when rebuilding from the same git revision (git
    describe yields the same dirty-tag both before and after).

    Returns 0 on confirmed install, 5 on "device back but slot didn't
    flip" (ie OTA was rejected; check serial), 4 on poll timeout."""
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
                    # Look at running_slot to confirm whether the OTA
                    # actually installed -- the unambiguous signal.
                    try:
                        c2 = _conn(host, port, timeout=5.0)
                        c2.request("GET", "/api/ota/status")
                        r2 = c2.getresponse()
                        st = json.loads(r2.read())
                        v    = st.get("app", {}).get("version", "?")
                        slot = st.get("running_slot", "?")
                        print(f"running version: {v} (slot {slot})")
                        c2.close()
                        if expect_slot_flip and slot == expect_slot_flip:
                            print("warning: running_slot didn't flip "
                                  f"({slot} unchanged) -- the upload was "
                                  "rejected by the firmware (likely "
                                  "image_validate, wrong_project, or "
                                  "ota_write). Check device serial logs.",
                                  file=sys.stderr)
                            return 5
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
