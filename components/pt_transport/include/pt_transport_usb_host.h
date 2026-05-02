#ifndef PT_TRANSPORT_USB_HOST_H
#define PT_TRANSPORT_USB_HOST_H

/*
 * pt_transport_usb_host — ESP32-S2 / ESP32-S3 native USB Host transport
 * for the PT-* raster command set.
 *
 * Uses the esp-idf usb_host component. Same VID/PID match as
 * pt_transport_libusb (Brother 0x04F9, PIDs {0x205E PT-H500,
 * 0x205F PT-E500, 0x2061 PT-P700, 0x2062 PT-P750W}). Same
 * pt_transport_t shape so pt_session works without knowing which
 * backend is below it.
 *
 * Target-only (esp-idf component build); the host CMake skips this
 * file. Both ESP32-S2 and ESP32-S3 expose the same USB-OTG controller,
 * so a single source supports both.
 */

#include <stdbool.h>
#include <stdint.h>

#include "pt_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pt_transport_usb_host pt_transport_usb_host_t;

/* Install the USB host stack, spawn lib + client tasks, and wait up to
 * connect_timeout_ms for a PT-* device to be enumerated. Returns NULL
 * on any failure (install failed, no matching device attached in time,
 * descriptor parse failed, etc.). */
pt_transport_usb_host_t *pt_transport_usb_host_open(uint32_t connect_timeout_ms);

/* Returns a pt_transport_t bound to *u, valid until close() is called. */
pt_transport_t pt_transport_usb_host_transport(pt_transport_usb_host_t *u);

/* Release the device, free transfers, stop tasks, uninstall stack. */
void pt_transport_usb_host_close(pt_transport_usb_host_t *u);

/* True when, during the most recent open() probe, a PT-* in P-Lite
 * mode (PIDs 0x2064 / 0x2065) was enumerated. The device exposes USB
 * Mass Storage in that state — no printer interface to bind to — so
 * open() returns NULL, but a caller can check this to render a
 * specific UI hint instead of generic "no printer". */
bool pt_transport_usb_host_plite_seen(void);

#ifdef __cplusplus
}
#endif

#endif /* PT_TRANSPORT_USB_HOST_H */
