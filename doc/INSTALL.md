# Install

How to flash a fresh ESP32-S3 devkit and get the SPA on your LAN.

See also: [HARDWARE.md](HARDWARE.md) for the parts list and cabling.

## First flash -- quick path

Run from a machine with the devkit's UART USB-C plugged in.

```sh
# 1. ESP-IDF v5.5+ in the shell
. ~/esp/esp-idf/export.sh                  # or: source scripts/idf-env.sh

# 2. (Optional) compile-in Wi-Fi creds. Skip this and use AP onboarding
#    instead -- see "Wi-Fi onboarding" below.
cp main/wifi_credentials.example.h main/wifi_credentials.h
$EDITOR main/wifi_credentials.h            # set WIFI_SSID / WIFI_PASSWORD

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

Then visit **`http://pt700.local/`** from any machine on the same LAN.
The status panel should show `transport: usb_host` (green) with real
model / tape values; press the print button to send a label.

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

If a status LED is wired (GPIO or WS2812 RGB pixel -- see
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

The devkit's BOOT button (GPIO 0) is the Wi-Fi-reset trigger. **Hold
it for 5+ seconds** while the device is running -- NVS creds get wiped
and the board reboots into AP-mode onboarding. Tap-press is harmless
(must be a sustained hold during runtime, not at reset).

To use a different GPIO, change `pt_app_config.reset_gpio_num` in
`main/main.c`. Negative value disables it.
