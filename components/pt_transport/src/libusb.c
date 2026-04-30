/*
 * pt_transport_libusb -- Linux/macOS userspace pt_transport_t, built on
 * libusb-1.0. See pt_transport_libusb.h.
 *
 * Compiled only when host CMake detects libusb-1.0 via pkg-config; the
 * ESP-IDF component CMakeLists.txt does not list this file.
 */

#include "pt_transport_libusb.h"

#include <libusb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BROTHER_VID 0x04F9
static const uint16_t KNOWN_PIDS[] = {
    0x205E,  /* PT-H500 */
    0x205F,  /* PT-E500 */
    0x2061,  /* PT-P700 */
};

struct pt_transport_libusb {
    libusb_context       *ctx;
    libusb_device_handle *handle;
    uint8_t               intf;
    uint8_t               ep_in;
    uint8_t               ep_out;
    bool                  detached_kernel;
};

/* Walk the active config and pick the first interface whose class is
 * PRINTER (0x07), recording its bulk-IN and bulk-OUT endpoint addresses. */
static int find_printer_interface(libusb_device *dev,
                                  uint8_t *intf_num,
                                  uint8_t *ep_in, uint8_t *ep_out)
{
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) return -1;

    int result = -1;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        if (iface->num_altsetting < 1) continue;
        const struct libusb_interface_descriptor *id = &iface->altsetting[0];
        if (id->bInterfaceClass != LIBUSB_CLASS_PRINTER) continue;

        bool got_in = false, got_out = false;
        for (int e = 0; e < id->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
            if ((ep->bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                *ep_in = ep->bEndpointAddress;
                got_in = true;
            } else {
                *ep_out = ep->bEndpointAddress;
                got_out = true;
            }
        }
        if (got_in && got_out) {
            *intf_num = id->bInterfaceNumber;
            result = 0;
            break;
        }
    }
    libusb_free_config_descriptor(cfg);
    return result;
}

static int lu_send(void *ctx, const uint8_t *data, size_t len)
{
    pt_transport_libusb_t *u = ctx;
    int actual = 0;
    int rc = libusb_bulk_transfer(u->handle, u->ep_out,
                                  (uint8_t *)(uintptr_t)data, (int)len,
                                  &actual, /*timeout_ms=*/ 5000);
    if (rc != 0) return -1;
    return actual;
}

static int lu_recv(void *ctx, uint8_t *out, size_t cap,
                   size_t *out_len, uint32_t timeout_ms)
{
    pt_transport_libusb_t *u = ctx;
    int actual = 0;
    int rc = libusb_bulk_transfer(u->handle, u->ep_in, out, (int)cap,
                                  &actual, (unsigned int)timeout_ms);
    if (rc == LIBUSB_ERROR_TIMEOUT) {
        *out_len = (size_t)actual;  /* possibly partial bytes */
        return 0;
    }
    if (rc != 0) { *out_len = 0; return -1; }
    *out_len = (size_t)actual;
    return 0;
}

pt_transport_libusb_t *pt_transport_libusb_open(void)
{
    pt_transport_libusb_t *u = calloc(1, sizeof *u);
    if (!u) return NULL;

    if (libusb_init(&u->ctx) != 0) { free(u); return NULL; }

    libusb_device **devs = NULL;
    ssize_t n = libusb_get_device_list(u->ctx, &devs);
    if (n < 0) goto fail;

    libusb_device *match = NULL;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(devs[i], &d) != 0) continue;
        if (d.idVendor != BROTHER_VID) continue;
        for (size_t j = 0; j < sizeof KNOWN_PIDS / sizeof KNOWN_PIDS[0]; j++) {
            if (d.idProduct == KNOWN_PIDS[j]) { match = devs[i]; break; }
        }
        if (match) break;
    }

    if (!match) { libusb_free_device_list(devs, 1); goto fail; }
    if (libusb_open(match, &u->handle) != 0) {
        libusb_free_device_list(devs, 1);
        goto fail;
    }
    libusb_free_device_list(devs, 1);

    if (find_printer_interface(libusb_get_device(u->handle),
                               &u->intf, &u->ep_in, &u->ep_out) != 0)
        goto close_fail;

    /* Linux's usblp grabs PRINTER-class devices by default -- detach so
     * we can claim. Save the fact for reattachment in close(). */
    if (libusb_kernel_driver_active(u->handle, u->intf) == 1) {
        if (libusb_detach_kernel_driver(u->handle, u->intf) == 0)
            u->detached_kernel = true;
    }

    if (libusb_claim_interface(u->handle, u->intf) != 0) goto close_fail;
    return u;

close_fail:
    libusb_close(u->handle);
    u->handle = NULL;
fail:
    if (u->ctx) libusb_exit(u->ctx);
    free(u);
    return NULL;
}

pt_transport_t pt_transport_libusb_transport(pt_transport_libusb_t *u)
{
    return (pt_transport_t){ .send = lu_send, .recv = lu_recv, .ctx = u };
}

void pt_transport_libusb_close(pt_transport_libusb_t *u)
{
    if (!u) return;
    if (u->handle) {
        libusb_release_interface(u->handle, u->intf);
        if (u->detached_kernel)
            libusb_attach_kernel_driver(u->handle, u->intf);
        libusb_close(u->handle);
    }
    if (u->ctx) libusb_exit(u->ctx);
    free(u);
}
