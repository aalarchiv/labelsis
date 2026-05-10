# Development

How to iterate on the firmware, the SPA, and the protocol layer
without flashing on every change.

## Mock server (no hardware needed)

```sh
python3 scripts/mock-server.py    # serves SPA + fake /api/* on :8080
# open http://localhost:8080/ in a browser
```

A single-file Python stdlib HTTP server returns sensible fake values
for every `/api/*` endpoint and serves the SPA from
`components/pt_app/spa/`. Print requests are validated (raster body
must be 16-byte aligned, etc.) and headers are logged so you can see
exactly what the SPA emits. Edit `STATE` at the top of
`scripts/mock-server.py` to simulate different printer conditions
(different tape size, error1/2 bits, transport=`mock`/`plite`/
`waiting`, etc.).

## SPA against a real device on the LAN

The firmware sends `Access-Control-Allow-Origin: *` on every API
response, so you can iterate the SPA from a localhost dev server
pointed at the actual ESP. No flash cycle per UI change.

```sh
cd components/pt_app/spa
python3 -m http.server 8080
# open http://localhost:8080/ in a browser
# Settings tab → API host = http://labelsis.local → save
# Print/Wi-Fi tabs now drive the real device
```

`localStorage["labelsis:apiHost"]` persists the setting; clear it via
the Settings tab to go back to same-origin.

## Architecture

Four layers, transport-agnostic:

```
pt_app  (Wi-Fi + HTTP API + mDNS + SPA + status LED + captive-portal DNS)
   ↓
pt_session  (job orchestration: print/cut/feed; status decode)
   ↓
pt_protocol  (raster encoders, status struct, tape geometry)
   ↓
pt_transport  (mock | libusb (host tests) | esp-idf usb_host)
```

`pt_transport_t` is a function-pointer struct (`send`/`recv`/`ctx`)
so the upper layers don't know which backend is below them. Linux
host tests run the same code with libusb; CI builds with `mock` on a
host CMake; the firmware uses `usb_host` on real hardware.

## Build matrix

```sh
./scripts/host-test.sh    # CMake/CTest host-side: protocol + session + mock
./scripts/ci.sh           # full: host tests + idf.py build for both S2/S3
idf.py build              # single target (current set-target)
```

`build-host/` and `build/` are gitignored and rebuilt as needed.

The firmware version baked into the binary comes from `git describe
--always --dirty --tags` at CMake configure time. Re-config triggers
on `.git/HEAD` and `.git/index` changes so the `--dirty` suffix stays
honest. The version is exposed to the SPA via `/api/info` and shown
in the About panel.

## CLI fallback (libusb)

`tools/cli/pt_send` is a Linux libusb pipeline that lets you talk to
a PT-* directly from a workstation, useful for validating wire-level
behaviour without the ESP. After `./scripts/host-test.sh`:

```sh
./build-host/tools_cli/pt_send -v --info               # tape geometry
./build-host/tools_cli/pt_send my-label.pbm            # print a 1-bit PBM
./build-host/tools_cli/pt_send --no-cut --chain ...    # see --help
```

Same protocol path as the firmware, just a different transport
(`pt_transport_libusb`).

## Files of interest

- `main/main.c` -- startup glue, `pt_app_config_t` defaults
- `components/pt_app/src/pt_app.c` -- Wi-Fi + HTTP API + AP onboarding + mDNS + OTA
- `components/pt_app/src/pt_dns.c` -- captive-portal DNS hijack (AP mode only)
- `components/pt_app/src/pt_led.c` -- status LED state machine
- `components/pt_app/spa/index.html` -- the SPA (vanilla HTML/CSS/JS)
- `components/pt_app/spa/setup.html` -- captive setup page (AP-mode only)
- `components/pt_transport/src/usb_host.c` -- esp-idf USB host transport
- `components/pt_transport/src/libusb.c` -- Linux/macOS userspace transport
- `components/pt_protocol/` -- raster encoders + status decoder + geometry
- `tools/cli/pt_send.c` -- Linux command-line printer driver

## OTA + release scripts

- `scripts/labelsis-ota.py` -- stdlib-only Python CLI that POSTs a
  built image to `/api/ota`, with progress, slot-flip detection, and
  reboot confirmation. Useful for headless / scripted flashing or
  when the SPA can't reach the device. See [doc/OTA.md](OTA.md).
- `scripts/build-release.sh` -- bundles every supported board into
  `release/<version>/` with both the OTA image (`*.bin`) and the
  merged single-image USB-flash blob (`*-merged.bin`), plus per-
  board `flash.sh` and a `SHA256SUMS` manifest.

## Verifying tape geometry against Brother's source

`scripts/parse_ptd.py` parses Brother's binary `BSPP*AD.PTD` driver
files (the per-model tape table the Windows driver loads at runtime).
Lets you cross-check the hardcoded geometry in
`components/pt_protocol/src/pt_protocol.c` against Brother's
authoritative numbers, or extend it for a new model:

```sh
scripts/parse_ptd.py path/to/BSPP70AD.PTD --compare
# matched=6  diffs=0  skipped=18 (split-print + HSe rows)
```

How to obtain a `.PTD` file is documented in the script's
docstring (download the printer driver MSI, unpack with 7z, look for
`BSPP*AD.PTD`).

## Issue tracker

This project uses **bd (beads)** for issue tracking. `bd ready` lists
open work; `bd show <id>` for details.
