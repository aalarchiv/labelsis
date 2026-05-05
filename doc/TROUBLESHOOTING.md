# Troubleshooting

First-hour symptom catalog.

| Symptom | Likely cause |
|---|---|
| No boot log on `idf.py monitor` | Wrong USB port -- use the **UART** port (left of devkit, near reset/boot buttons), not the native USB (right). See [HARDWARE.md](HARDWARE.md#devkit-usb-ports). |
| `wifi: failed` in log | Wrong `WIFI_SSID` / `WIFI_PASSWORD` in `main/wifi_credentials.h`, or `WIFI_AUTH_WPA2_PSK` mismatch (open / WPA3-only networks not supported). Or just delete the file and use AP-mode onboarding. |
| `transport: waiting -- no PT-* attached` | Printer not plugged in, plugged into the wrong port (use the **right** USB-C / OTG port for the printer), or printer powered off. SPA still loads; printer-state UI shows the issue. |
| `transport: P-Lite mode …` | Side slider in EL position. Slide to E or hold the printer's PLite button for 2 s; the ESP picks up the re-enumerated device automatically within a few seconds. Details in [USAGE.md](USAGE.md#p-lite-mode). |
| Print returns timeout | If `/api/status` shows `transport: usb_host` (not `mock`), and the printer has tape, check `print_timeout_ms` in `pt_app_config_t` -- default 120 s suffices for ~3 m of label. |
| `labelsis.local` doesn't resolve | mDNS support missing on your OS (rare on macOS/Linux, common on Windows without iTunes/Bonjour) or blocked by your router (enterprise / mesh client-isolation). Use the printed IP from the boot log, or check the router's DHCP table for `labelsis`. |
| `httpd_register_uri_handler: no slots` | `cfg.max_uri_handlers` in `http_up()` is too low. Bump it. Currently 16. |
| Build complains about PSRAM / flash size / partition overflow on the wrong chip | You probably switched targets without nuking the build. `idf.py` only consumes `SDKCONFIG_DEFAULTS` (and the per-board overlay) on initial config. Run `rm -rf build sdkconfig && idf.py -D BOARD=<name> set-target <s2\|s3> && idf.py build`. See [boards/README.md](../boards/README.md). |
| Status LED stays off | Either `.led.type = PT_LED_TYPE_NONE`, or the configured GPIO is wrong for your board. Default is GPIO 48 (DevKitC-1 onboard RGB); check `pt_app_config_t.led` in `main/main.c`. |
| Captive portal doesn't pop on phone | iOS/Android probes time out before the DNS server is up if the device just booted. Disconnect + reconnect to `labelsis-setup` after a few seconds. If that still fails, navigate to `http://192.168.4.1/` manually. |
| Cannot connect to `labelsis-setup` AP | The AP is open (no password) by design for first-boot. If your phone insists on a password, choose "Skip / no internet" when prompted. |
| BOOT-button reset doesn't seem to work | The default GPIO 0 is the devkit's BOOT button, but it must be held during runtime (not at reset/power-on). Hold past 5 seconds **without releasing** to wipe creds; if you release between 3 and 5 s you'll get the OTA-gate toggle instead. The serial log narrates both transitions. |
