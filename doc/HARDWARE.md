# Hardware

What to buy and how to wire it.

| Item | Notes |
|---|---|
| **ESP32-S3-DevKitC-1-N16R8** | 16 MB QIO flash, 8 MB octal PSRAM. The `sdkconfig.defaults.esp32s3` overlay is tuned for this board; an `N8R2` (8 MB flash, 2 MB quad PSRAM) board needs the flash-size and `SPIRAM_MODE_OCT` lines overridden. |
| **PT-P700-family label printer** | PT-H500, PT-P700, PT-P750W, PT-E500. Models with a side slider must be in the normal position (**E** / Edit), not **EL** (Editor Lite / P-Lite mode); see [USAGE.md](USAGE.md) for P-Lite handling. |
| **USB-C → USB-B cable** | From the devkit's right-side USB port (native S3 USB-OTG peripheral) to the printer's square USB-B port. The devkit becomes the host. A USB-C-to-USB-A adapter + a normal A-to-B printer cable also works. |
| **USB-C → USB-A or USB-C → USB-C** | From the devkit's left-side UART port (CP2102N) to your PC, for flashing + serial logs. The two USB-C ports on the devkit are independent; both can be plugged in simultaneously. |
| **Printer power supply** | The printer is self-powered from its own AC adapter, not the devkit. |

## Devkit USB ports

The ESP32-S3-DevKitC-1 has two USB-C connectors. They are **not**
interchangeable:

- **Left** (near reset/boot buttons) -- CP2102N UART bridge. Use this
  for flashing and `idf.py monitor`. This is the port your PC needs
  to see.
- **Right** -- the S3's native USB-OTG peripheral. This is where the
  printer plugs in; the devkit becomes the host.

Both can be plugged in at the same time without conflict. A common
mistake on first flash is plugging into the wrong port and getting
either no boot log or no printer detection.

## Status LED

The default `pt_app_config_t.led` config in `main/main.c` drives the
DevKitC-1's onboard WS2812 RGB pixel on GPIO 48. For boards without
that pixel, switch to a plain GPIO LED:

```c
.led = { .type = PT_LED_TYPE_GPIO, .gpio = 2, .active_low = false },
```

Or disable entirely:

```c
.led = { .type = PT_LED_TYPE_NONE },
```

LED cadence/colour table is in [INSTALL.md](INSTALL.md#status-led).

## ESP32-S2 alternative

The ESP32-S2-DevKitC-1 builds clean (`idf.py set-target esp32s2`)
because the USB-OTG IP block is the same. Use the
`sdkconfig.defaults.esp32s2` overlay instead. Final S2-vs-S3 ship
target is open in the issue tracker (`pt700-bzf`).
