# Hardware

What to buy and how to wire it.

| Item | Notes |
|---|---|
| **ESP32-S3-DevKitC-1-N16R8** *or* **Wemos LOLIN S2 Mini** | Reference + tested: DevKitC-1 (board profile `devkitc_s3`). Compact alternative: LOLIN S2 Mini (`lolin_s2_mini`, plain LED on GPIO 15, 4 MB flash, no PSRAM). Other S2 / S3 boards work too; see [boards/README.md](../boards/README.md) for adding a profile. |
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

The LED type, pin, and polarity come from the active board profile
(`boards/<name>/board.h`). Shipped profiles:

- `devkitc_s3` -- WS2812 RGB pixel on GPIO 48 (DevKitC-1 onboard)
- `lolin_s2_mini` -- plain LED on GPIO 15 (S2 Mini onboard)

To disable on a board with no usable LED, set
`#define BOARD_LED_TYPE PT_LED_TYPE_NONE` in that profile's `board.h`.

LED cadence/colour table is in [INSTALL.md](INSTALL.md#status-led).

## Adding a new board

The board profile is a folder under `boards/<name>/` with three files
(`board.h`, `sdkconfig.defaults`, `partitions.csv`). The full guide is
[boards/README.md](../boards/README.md). Build with
`idf.py -D BOARD=<name> set-target <s2|s3>`.

The S3-vs-S2 final-target decision (`pt700-bzf`) becomes a non-issue
under board profiles -- both ship.
