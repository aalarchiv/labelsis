/* Wemos LOLIN S2 Mini -- ESP32-S2FH4: 4 MB embedded flash, no PSRAM,
 * no Bluetooth radio. Single plain LED on GPIO 15 (active-high).
 * BOOT button on GPIO 0. */
#pragma once

#include "pt_led.h"

#define BOARD_NAME            "Wemos LOLIN S2 Mini"
#define BOARD_LED_TYPE        PT_LED_TYPE_GPIO
#define BOARD_LED_GPIO        15
#define BOARD_LED_ACTIVE_LOW  false
#define BOARD_RESET_GPIO      0    /* BOOT button -- 5 s hold wipes Wi-Fi creds */
