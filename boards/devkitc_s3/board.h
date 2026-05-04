/* ESP32-S3-DevKitC-1-N16R8 -- 16 MB QIO flash, 8 MB octal PSRAM.
 * Onboard WS2812 RGB pixel on GPIO 48; BOOT button on GPIO 0.
 * Reference platform; tested on hardware. */
#pragma once

#include "pt_led.h"

#define BOARD_NAME            "ESP32-S3-DevKitC-1-N16R8"
#define BOARD_LED_TYPE        PT_LED_TYPE_RGB
#define BOARD_LED_GPIO        48
#define BOARD_LED_ACTIVE_LOW  false
#define BOARD_RESET_GPIO      0    /* BOOT button -- 5 s hold wipes Wi-Fi creds */
