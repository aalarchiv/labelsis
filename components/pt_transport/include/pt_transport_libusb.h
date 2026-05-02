#ifndef PT_TRANSPORT_LIBUSB_H
#define PT_TRANSPORT_LIBUSB_H

/*
 * pt_transport_libusb — Linux/macOS userspace transport for the PT-*
 * raster command set, built on libusb-1.0.
 *
 * Lets us drive the printer from a host laptop without ESP-IDF in the
 * loop — pt_session + pt_protocol get exercised against real hardware
 * before the ESP32-S3 USB host stack is involved, isolating
 * protocol bugs from USB-driver bugs.
 *
 * Host-only; the ESP-IDF component build never compiles this file (the
 * component CMakeLists.txt does not list it). Host CMake gates on
 * pkg-config libusb-1.0 detection.
 */

#include "pt_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pt_transport_libusb pt_transport_libusb_t;

/* Open the first connected PT-* device (VID 0x04F9, PID in
 * {0x205E PT-H500, 0x205F PT-E500, 0x2061 PT-P700, 0x2062 PT-P750W}).
 * Returns NULL on libusb init failure or when no matching device is
 * found. */
pt_transport_libusb_t *pt_transport_libusb_open(void);

/* Get a pt_transport_t bound to *u for use by pt_session etc. The
 * pt_transport_t is valid until pt_transport_libusb_close() is called. */
pt_transport_t pt_transport_libusb_transport(pt_transport_libusb_t *u);

/* Release the interface, reattach the Linux usblp kernel driver if we
 * detached it, then libusb_exit. Frees *u. */
void pt_transport_libusb_close(pt_transport_libusb_t *u);

#ifdef __cplusplus
}
#endif

#endif /* PT_TRANSPORT_LIBUSB_H */
