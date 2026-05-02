#ifndef PT_PLITE_UNSTICK_H
#define PT_PLITE_UNSTICK_H

/*
 * Programmatic flip of a PT-* in P-Lite (Editor Lite) mode back to
 * normal print mode (PID 0x2061), without the user having to touch
 * the slider.
 *
 * Mechanism: the printer firmware in EL mode exposes a small FAT
 * volume containing the file PTLITE.PRN. When that file is rewritten
 * such that its last 10 bytes match the magic sequence
 *   1B 69 61 01  1B 69 55 66  00 00
 * the firmware triggers an EL->E transition and the device
 * re-enumerates as the printer-class PID 0x2061 within ~10 s. We
 * just write the file via FatFs on top of espressif/usb_host_msc.
 *
 * Mechanism reverse-engineered from Brother's BrUsbPrnIO.dll
 * (BrEsSwitchELtoETempW); see bd memory
 * pt-p-lite-mode-unstick-programmatic-flip-is for the full notes.
 *
 * Internal to pt_transport. usb_host.c calls plite_unstick_init()
 * during host_init_once and plite_unstick_schedule(addr) when the
 * client_event handler sees a P-Lite PID enumerate.
 */

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the MSC class driver. Idempotent. Spawns a background
 * task for MSC USB events. Costs ~5 KB stack + a small ringbuf. */
esp_err_t plite_unstick_init(void);

/* One-shot worker task that mounts the MSC volume at the given USB
 * address, rewrites PTLITE.PRN, unmounts, and exits. Returns
 * immediately after spawning the task. */
esp_err_t plite_unstick_schedule(uint8_t dev_addr);

#ifdef __cplusplus
}
#endif

#endif /* PT_PLITE_UNSTICK_H */
