# pt700

Network print server for the Brother **PT-P700** family of label printers,
running on **ESP32-S3** (USB host) with a self-hosted web UI for label
design.

## Supported printers

Same protocol family per Brother's SDM v1.11 (`PT-H500/P700/E500 raster
command reference`); PT-P750W cross-confirmed against ptouch-esp32. All
must be in normal print mode (slider position **E** on models that have
one, not **EL** / P-Lite).

| Model     | VID:PID    | Notes |
|-----------|------------|-------|
| PT-H500   | 04F9:205E  | Handheld |
| PT-P700   | 04F9:2061  | Reference platform; tested |
| PT-P750W  | 04F9:2062  | P700 with on-printer Wi-Fi (irrelevant when wired via USB) |
| PT-E500   | 04F9:205F  | Handheld |

P-Lite-mode PIDs (`0x2064` / `0x2065`) are detected and surfaced to the
SPA with a "flip the slider" hint, but cannot be driven directly --
they enumerate as USB Mass Storage, not printer class.

## What it does

- **USB host** — drives the printer over the ESP32-S3's native USB-OTG
  controller using the SDM raster command set.
- **Web UI** — single-page label designer at `http://pt700.local/` with live
  tape-aware preview, status panel, CSV-templated batch printing, and Wi-Fi
  / settings tabs.
- **Wi-Fi onboarding** — falls back to a SoftAP (`pt700-setup`) with a
  captive setup page when no creds are provisioned; long-press BOOT button
  on the devkit to wipe and re-onboard.
- **mDNS** — advertises `pt700.local` on the LAN.

## Hardware

| Item | Notes |
|---|---|
| **ESP32-S3-DevKitC-1-N16R8** | 16 MB QIO flash, 8 MB octal PSRAM. The `sdkconfig.defaults.esp32s3` overlay is tuned for this board; an `N8R2` (8 MB flash, 2 MB quad PSRAM) board needs the flash-size and `SPIRAM_MODE_OCT` lines overridden. |
| **PT-P700-family label printer** | See *Supported printers* above for the full PID list. Models with a side slider must be in the normal position (**E** / Edit), not **EL** (Editor Lite / P-Lite mode); see *P-Lite* below. |
| **USB-C → USB-B cable** | From the devkit's right-side USB port (native S3 USB-OTG peripheral) to the printer's square USB-B port. The devkit becomes the host. A USB-C-to-USB-A adapter + a normal A-to-B printer cable also works. |
| **USB-C → USB-A or USB-C → USB-C** | From the devkit's left-side UART port (CP2102N) to your PC, for flashing + serial logs. The two USB-C ports on the devkit are independent; both can be plugged in simultaneously. |
| **PT-P700 power supply** | Printer self-powered from its own AC adapter, not the devkit. |

ESP32-S2 (DevKitC-1) is also supported and builds clean (`idf.py set-target
esp32s2`); pick which to ship is `pt700-bzf` in the issue tracker.

## First flash — quick path

Done from a machine with the devkit's UART USB-C plugged in.

```sh
# 1. ESP-IDF v5.5+ in the shell
source scripts/idf-env.sh                 # or: . ~/esp/esp-idf/export.sh

# 2. Wi-Fi creds (gitignored — copy + edit)
cp main/wifi_credentials.example.h main/wifi_credentials.h
$EDITOR main/wifi_credentials.h           # set WIFI_SSID / WIFI_PASSWORD

# 3. Target + build + flash + monitor
idf.py set-target esp32s3
idf.py build
idf.py flash monitor                       # -p /dev/ttyUSB0 if auto-detect picks wrong
                                           # Ctrl+] to exit monitor
```

Healthy boot log:

```
I (...) pt_app: wifi: connecting to <SSID> (from cfg)
I (...) pt_app: got IP: 192.168.x.y
I (...) pt_app: wifi: persisted creds to NVS
I (...) pt_usb: PT-* paired: vid=04f9 pid=2061 intf=0 in=0x81 out=0x02
I (...) pt_app: transport: usb_host (PT-* attached)
I (...) pt_app: mdns: pt700.local up
I (...) pt_app: http: serving on port 80
```

Then visit **`http://pt700.local/`** from any machine on the same LAN. The
status panel should show `transport: usb_host` (green) with real model /
tape values; press the print button to send a label.

## Wi-Fi onboarding (no `wifi_credentials.h`)

If `main/wifi_credentials.h` is absent, the configured SSID is out of
range, or the password is wrong, the device brings up an open SoftAP
`pt700-setup`. Same fallback if the network later disappears (router
swap, password change, moving the device).

1. Connect a phone or laptop to **`pt700-setup`**.
2. The phone's "sign in to network" sheet should pop straight onto the
   setup page, thanks to the on-device captive-portal DNS. If it
   doesn't, open `http://192.168.4.1/` manually.
3. Pick your home network from the scan dropdown (or type SSID), enter
   the WPA2 passphrase twice (with the `show` toggle if needed), hit
   **save & reboot**.
4. Reconnect to your normal Wi-Fi; reload `http://pt700.local/`.

After a successful first STA association the creds are persisted to
NVS and the AP doesn't come back up unless STA fails again or you wipe.

mDNS (`pt700.local`) needs Bonjour on Windows < 10 and is sometimes
blocked on enterprise / mesh networks with client isolation. If
`pt700.local` doesn't resolve, look for the device's IP in your
router's DHCP table and use that directly.

## Status LED

If a status LED is wired (GPIO or WS2812 RGB pixel — see
`pt_app_config.led` in `main/main.c`), it indicates which stage the
device is in. Default config matches the ESP32-S3-DevKitC-1's onboard
RGB pixel on GPIO 48.

| Stage           | Cadence                | RGB colour |
|-----------------|------------------------|------------|
| Boot            | very fast strobe       | yellow     |
| STA connecting  | brief blip every 1 s   | blue       |
| AP onboarding   | fast even blink (~3 Hz)| magenta    |
| Ready           | solid                  | green      |
| USB waiting     | brief blip every 2 s   | orange     |
| Printing        | rapid strobe           | white      |
| Error           | even 1 Hz blink        | red        |

A single-GPIO LED is distinguished by cadence alone (colour collapses
to "on/off"). Set `.led.type = PT_LED_TYPE_NONE` to disable.

## BOOT button = Wi-Fi reset

The devkit's BOOT button (GPIO 0) is the Wi-Fi-reset trigger. **Hold it for
5+ seconds** while the device is running — NVS creds get wiped and the
board reboots into AP-mode onboarding. Tap-press is harmless (must be a
sustained hold during runtime, not at reset).

To use a different GPIO, change `pt_app_config.reset_gpio_num` in
`main/main.c`. Negative value disables it.

## P-Lite mode

The PT-P700 has a side slider with two positions:

- **E** (normal Edit mode) — exposes a USB printer-class interface; this is
  what the firmware drives.
- **EL** (Editor Lite / P-Lite) — re-enumerates as USB Mass Storage
  (PID `0x2064`) hosting Brother's Windows-only "Editor Lite" tool. The
  firmware **cannot drive prints** in this mode.

If the slider is in EL position, the SPA shows
`transport: P-Lite mode — slide switch to E or hold PLite button 2s, then
reboot`. Either physically slide the switch or hold the printer's PLite
button for ~2 s to flip back, then reboot the ESP32. Driving the printer
out of P-Lite over USB alone is tracked as `pt700-wmf` (P4) — needs a USB
packet capture from Brother's own tool to get right.

## Developing the SPA without flashing

Two options, depending on whether you want to talk to a real device or
not.

**Option A — mock server (no hardware needed):**

```sh
python3 scripts/mock-server.py    # serves SPA + fake /api/* on :8080
# open http://localhost:8080/ in a browser
```

A single-file Python stdlib HTTP server returns sensible fake values
for every `/api/*` endpoint and serves the SPA from `components/pt_app/
spa/`. Print requests are validated (raster body must be 16-byte
aligned, etc.) and headers are logged so you can see exactly what the
SPA emits. Edit `STATE` at the top of `scripts/mock-server.py` to
simulate different printer conditions (different tape size, error1/2
bits, transport=`mock`/`plite`, etc.).

**Option B — point the SPA at a real device on the LAN:**

The firmware sends `Access-Control-Allow-Origin: *` on every API
response, so you can iterate the SPA from a localhost dev server
pointed at the actual ESP. No flash cycle per UI change.

```sh
cd components/pt_app/spa
python3 -m http.server 8080
# open http://localhost:8080/ in a browser
# Settings tab → API host = http://pt700.local → save
# Print/Wi-Fi tabs now drive the real device
```

`localStorage["pt700:apiHost"]` persists the setting; clear it via the
Settings tab to go back to same-origin.

## Architecture (1-line summary)

Four layers, transport-agnostic:

```
pt_app  (Wi-Fi + HTTP API + mDNS + SPA)
   ↓
pt_session  (job orchestration: print/cut/feed; status decode)
   ↓
pt_protocol  (raster encoders, status struct, tape geometry)
   ↓
pt_transport  (mock | libusb (host tests) | esp-idf usb_host)
```

`pt_transport_t` is a function-pointer struct (`send`/`recv`/`ctx`) so the
upper layers don't know which backend is below them. Linux host tests run
the same code with libusb; CI builds with `mock` on a host CMake; the
firmware uses `usb_host` on real hardware.

## Build matrix

```sh
./scripts/host-test.sh    # CMake/CTest host-side: protocol + session + mock
./scripts/ci.sh           # full: host tests + idf.py build for both S2/S3
idf.py build              # single target (current set-target)
```

`build-host/` and `build/` are gitignored and rebuilt as needed.

## CLI fallback

`tools/cli/pt_send` is a Linux libusb pipeline that lets you talk to a
PT-P700 directly from a workstation, useful for validating wire-level
behaviour without the ESP. After `./scripts/host-test.sh`:

```sh
./build-host/tools_cli/pt_send -v --info               # tape geometry
./build-host/tools_cli/pt_send my-label.pbm            # print a 1-bit PBM
./build-host/tools_cli/pt_send --no-cut --chain ...    # see --help
```

Same protocol path as the firmware, just a different transport
(`pt_transport_libusb`).

## Troubleshooting first-hour

| Symptom | Likely cause |
|---|---|
| No boot log on `idf.py monitor` | Wrong USB port — use the **UART** port (left of devkit, near reset/boot buttons), not the native USB (right). |
| `wifi: failed` in log | Wrong `WIFI_SSID` / `WIFI_PASSWORD` in `main/wifi_credentials.h`, or `WIFI_AUTH_WPA2_PSK` mismatch (open / WPA3-only networks not supported). |
| `transport: mock (no printer)` | Printer not plugged in, plugged into the wrong port (use the **right** USB-C / OTG port for the printer), or printer powered off. |
| `transport: P-Lite mode …` | Side slider in EL position. Slide to E or hold PLite button 2 s, then reboot the ESP. |
| Print returns timeout | Was a real bug fixed in commit `456773b` (session-mutex). If it still happens, check that `/api/status` returns `transport: usb_host` and not `mock`. |
| `pt700.local` doesn't resolve | mDNS support missing on your OS (rare on macOS/Linux, common on Windows without iTunes/Bonjour). Use the printed IP from the boot log. |
| `httpd_register_uri_handler: no slots` | Build is older than commit `28a6dcd`; rebuild. |
| ESP32-S2 build complains about PSRAM | The `sdkconfig.defaults.esp32s3` overlay enables octal PSRAM that S2 lacks. Use `idf.py set-target esp32s2` (different overlay) — don't reuse a build dir across targets. |

## Files of interest

- `main/main.c` — startup glue, `pt_app_config_t` defaults
- `components/pt_app/src/pt_app.c` — Wi-Fi + HTTP API + AP onboarding + mDNS
- `components/pt_app/spa/index.html` — the SPA (vanilla HTML/CSS/JS)
- `components/pt_app/spa/setup.html` — captive setup page (AP-mode only)
- `components/pt_transport/src/usb_host.c` — esp-idf USB host transport
- `components/pt_transport/src/libusb.c` — Linux/macOS userspace transport
- `components/pt_protocol/` — raster encoders + status decoder + geometry
- `tools/cli/pt_send.c` — Linux command-line printer driver

## Issue tracker

This project uses **bd (beads)** for issue tracking. `bd ready` lists open
work; `bd show <id>` for details. Phase 6 (hardware bring-up) is mostly
done as of `pt700-xz2`; remaining ready work includes the AP-mode HW
verify (`pt700-9wa`), mDNS cross-OS (`pt700-7ge`), 24-h soak
(`pt700-o8q`), and the S2-vs-S3 final-target decision (`pt700-bzf`).
