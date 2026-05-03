/*
 * pt_transport_usb_host -- esp-idf USB Host backend for pt_transport_t.
 * See pt_transport_usb_host.h for the API.
 *
 * Target-only -- the host CMake build does not list this file. Compiles
 * for both ESP32-S2 and ESP32-S3 (identical USB-OTG controller).
 *
 * Lifecycle: usb_host_install + the lib/client tasks are one-shot
 * singletons. The first call to pt_transport_usb_host_open() brings
 * the stack up; subsequent retries (after a no-device timeout) just
 * wait again on the device-ready semaphore. We never call
 * usb_host_uninstall in production -- it's brittle to re-install
 * during the same boot, and the firmware never tears the stack down
 * anyway. Hot-plug after boot now works because the client task is
 * already running when the device's NEW_DEV event arrives.
 */

#include "pt_transport_usb_host.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "pt_usb";

#define PT_VID 0x04F9
static const uint16_t PT_PIDS[] = {
    0x205E,  /* PT-H500 */
    0x205F,  /* PT-E500 */
    0x2061,  /* PT-P700 */
    0x2062,  /* PT-P750W (Wi-Fi sibling of P700; identical USB protocol
              * when wired -- the on-printer Wi-Fi is independent of the
              * print path). Confirmed by ptouch-esp32 PID table flags
              * (FLAG_RASTER_PACKBITS|FLAG_P700_INIT, same as P700). */
};

/* Same VID, different firmware mode: when the side slider is in P-Lite
 * position the PT-* enumerates as USB Mass Storage (a virtual disk
 * containing Brother's "P-touch Editor Lite") with one of these PIDs
 * and exposes no printer-class interface. We can't drive prints in
 * that state today; flagging it lets the SPA show the user a useful
 * "slide to E or hold the PLite button" hint instead of generic
 * "unavailable".
 *
 * Programmatic auto-unstick was attempted three times (commits
 * 85a015b, 86fab68, d27ff27) and reverted each time -- our writes
 * reach the printer's PTLITE.PRN file but the firmware doesn't act
 * on them. The MSC handler that processes those writes lives in a
 * separate firmware partition not present in any of the firmware
 * blobs we have, so we cannot reverse-engineer the trigger from
 * static analysis alone. See doc/RESEARCH-PLITE.md for the full
 * post-mortem and what would unblock this (a USB packet capture of
 * Brother's BrEsSwitchELtoETempW running against a real device). */
static const uint16_t PT_PIDS_PLITE[] = {
    0x2064,  /* PT-P700 in P-Lite mode */
    0x2065,  /* PT-P750W in P-Lite mode */
};

/* Sticky flag set by on_client_event when a P-Lite-mode PT-* shows up.
 * The pt_app retry loop reads this between open() attempts to decide
 * whether to surface a P-Lite-specific message instead of the generic
 * "no printer attached". */
static volatile bool s_plite_seen;

/* Bulk transfers: PT-* MPS is 64 bytes per the SDM. We allocate 1 KB
 * per direction so a status read can request 64 (one MPS) and an OUT
 * write can fit a multi-row chunk. */
#define USB_BUF_SIZE 1024

struct pt_transport_usb_host {
    SemaphoreHandle_t        bus_lock;        /* serializes send/recv     */
    SemaphoreHandle_t        xfer_in_done;    /* signaled by IN callback  */
    SemaphoreHandle_t        xfer_out_done;   /* signaled by OUT callback */

    usb_device_handle_t      dev_hdl;
    uint8_t                  dev_addr;
    uint8_t                  intf_num;
    uint8_t                  ep_in_addr;
    uint8_t                  ep_out_addr;
    uint16_t                 ep_in_mps;
    uint16_t                 ep_out_mps;

    usb_transfer_t          *xfer_in;
    usb_transfer_t          *xfer_out;

    bool                     device_open;
    bool                     interface_claimed;
};

/* Singleton USB host stack. Installed lazily on first open(). */
static bool                     s_host_inited;
static usb_host_client_handle_t s_client_hdl;
static TaskHandle_t             s_lib_task;
static TaskHandle_t             s_client_task;

/* Filtering pipeline between the client event handler (producer) and
 * pt_transport_usb_host_open() (consumer). When a PT-* shows up, the
 * handler opens the device, validates VID/PID, stores the handle in
 * s_pending_dev, and releases s_pending_sem. The consumer takes the
 * semaphore + claims the handle. s_pending_lock guards both fields. */
static SemaphoreHandle_t        s_pending_sem;
static SemaphoreHandle_t        s_pending_lock;
static usb_device_handle_t      s_pending_dev;
static uint8_t                  s_pending_addr;

/* The currently-paired transport, or NULL. Used by the client event
 * handler to invalidate it on USB_HOST_CLIENT_EVENT_DEV_GONE so any
 * in-flight or subsequent send/recv on a yanked cable fails cleanly
 * instead of dereferencing a stale device handle. */
static struct pt_transport_usb_host *volatile s_active;

/* tasks */

static void lib_task(void *arg)
{
    (void)arg;
    /* Loop forever. We don't expose a teardown -- the firmware keeps
     * this running for the lifetime of the boot. */
    while (1) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(100), &flags);
    }
}

static void client_task(void *arg)
{
    (void)arg;
    while (1) {
        usb_host_client_handle_events(s_client_hdl, pdMS_TO_TICKS(100));
    }
}

/* descriptor walk */

/* Search the active config for a printer-class interface (0x07) with
 * one bulk-IN and one bulk-OUT endpoint. Records intf_num + endpoint
 * addresses + MPS. Returns 0 on success, -1 otherwise. */
static int pick_endpoints(struct pt_transport_usb_host *u)
{
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(u->dev_hdl, &cfg) != ESP_OK || !cfg)
        return -1;

    const uint8_t *p   = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;
    p += cfg->bLength;

    bool                                 in_target_intf = false;
    const usb_intf_desc_t               *cur_intf       = NULL;
    bool                                 got_in         = false;
    bool                                 got_out        = false;

    while (p + 2 <= end) {
        uint8_t bLength         = p[0];
        uint8_t bDescriptorType = p[1];
        if (bLength < 2 || p + bLength > end) break;

        if (bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)p;
            if (intf->bInterfaceClass == USB_CLASS_PRINTER) {
                in_target_intf = true;
                cur_intf       = intf;
                got_in = got_out = false;
            } else {
                in_target_intf = false;
            }
        } else if (in_target_intf
                   && bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)p;
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK)
                == USB_BM_ATTRIBUTES_XFER_BULK) {
                if (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                    u->ep_in_addr = ep->bEndpointAddress;
                    u->ep_in_mps  = ep->wMaxPacketSize;
                    got_in        = true;
                } else {
                    u->ep_out_addr = ep->bEndpointAddress;
                    u->ep_out_mps  = ep->wMaxPacketSize;
                    got_out        = true;
                }
                if (got_in && got_out && cur_intf) {
                    u->intf_num = cur_intf->bInterfaceNumber;
                    return 0;
                }
            }
        }
        p += bLength;
    }
    return -1;
}

/* enumeration callback */

static void on_client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        usb_device_handle_t dev = NULL;
        if (usb_host_device_open(s_client_hdl, msg->new_dev.address, &dev) != ESP_OK)
            return;

        const usb_device_desc_t *desc = NULL;
        if (usb_host_get_device_descriptor(dev, &desc) != ESP_OK || !desc) {
            usb_host_device_close(s_client_hdl, dev);
            return;
        }

        bool match = (desc->idVendor == PT_VID);
        if (match) {
            match = false;
            for (size_t i = 0; i < sizeof PT_PIDS / sizeof PT_PIDS[0]; i++)
                if (desc->idProduct == PT_PIDS[i]) { match = true; break; }
        }

        if (!match) {
            /* Same vendor, P-Lite mode? Flag for the app layer so the
             * SPA can render a specific hint instead of generic
             * "unavailable". Three auto-unstick attempts (commits
             * 85a015b, 86fab68, d27ff27) all reverted -- bytes reach
             * the printer's PTLITE.PRN at the right offset but the
             * firmware doesn't act on them, and the MSC handler that
             * processes those writes lives in a firmware partition we
             * don't have. See doc/RESEARCH-PLITE.md. */
            if (desc->idVendor == PT_VID) {
                for (size_t i = 0; i < sizeof PT_PIDS_PLITE / sizeof PT_PIDS_PLITE[0]; i++) {
                    if (desc->idProduct == PT_PIDS_PLITE[i]) {
                        ESP_LOGW(TAG, "PT-* in P-Lite mode (pid=%04x) -- "
                                      "slide to E or hold PLite button 2s",
                                 desc->idProduct);
                        s_plite_seen = true;
                        break;
                    }
                }
            }
            usb_host_device_close(s_client_hdl, dev);
            return;
        }

        /* PT-* found. Hand it to whoever's currently waiting in
         * pt_transport_usb_host_open. The lock guards against two
         * NEW_DEV events racing (rare but possible if a hub bursts
         * connect events). */
        if (xSemaphoreTake(s_pending_lock, portMAX_DELAY) == pdTRUE) {
            if (s_pending_dev) {
                /* Previous candidate hadn't been consumed yet; drop
                 * the older one so the newest device wins. Probably
                 * a duplicate event. */
                usb_host_device_close(s_client_hdl, s_pending_dev);
            }
            s_pending_dev  = dev;
            s_pending_addr = msg->new_dev.address;
            xSemaphoreGive(s_pending_lock);
            xSemaphoreGive(s_pending_sem);
            ESP_LOGI(TAG, "PT-* enumerated: vid=%04x pid=%04x addr=%u",
                     desc->idVendor, desc->idProduct, msg->new_dev.address);
        } else {
            usb_host_device_close(s_client_hdl, dev);
        }
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        /* Device disconnected. Two cases:
         *  - It was the active session's device: mark the session
         *    closed so subsequent send/recv fails fast and the SPA's
         *    next status poll sees transport=waiting; pt_app's retry
         *    task picks up the next attach.
         *  - It was a pending (filtered but not yet claimed) device:
         *    drop our stored handle so the next open() doesn't try
         *    to claim a dead device. */
        struct pt_transport_usb_host *u = s_active;
        if (u && u->dev_hdl == msg->dev_gone.dev_hdl) {
            if (u->interface_claimed) {
                usb_host_interface_release(s_client_hdl, u->dev_hdl, u->intf_num);
                u->interface_claimed = false;
            }
            usb_host_device_close(s_client_hdl, u->dev_hdl);
            u->dev_hdl     = NULL;
            u->device_open = false;
            ESP_LOGW(TAG, "PT-* disconnected");
        }
        if (xSemaphoreTake(s_pending_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_pending_dev == msg->dev_gone.dev_hdl) {
                s_pending_dev = NULL;
            }
            xSemaphoreGive(s_pending_lock);
        }
    }
}

/* Install the USB host stack, register our client, and spawn the lib /
 * client tasks. Idempotent -- safe to call from every open() attempt
 * because retries during a no-device wait must not re-install. */
static esp_err_t host_init_once(void)
{
    if (s_host_inited) return ESP_OK;

    s_pending_sem  = xSemaphoreCreateBinary();
    s_pending_lock = xSemaphoreCreateMutex();
    if (!s_pending_sem || !s_pending_lock) {
        ESP_LOGE(TAG, "host_init_once: semaphore alloc failed");
        return ESP_ERR_NO_MEM;
    }

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t r = usb_host_install(&host_cfg);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(r));
        return r;
    }

    if (xTaskCreate(lib_task, "pt_usb_lib", 4096, NULL, 5, &s_lib_task) != pdPASS) {
        ESP_LOGE(TAG, "lib_task create failed");
        return ESP_FAIL;
    }

    const usb_host_client_config_t cli_cfg = {
        .is_synchronous   = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = on_client_event,
            .callback_arg          = NULL,
        },
    };
    if (usb_host_client_register(&cli_cfg, &s_client_hdl) != ESP_OK) {
        ESP_LOGE(TAG, "client_register failed");
        return ESP_FAIL;
    }

    if (xTaskCreate(client_task, "pt_usb_cli", 4096, NULL, 5, &s_client_task) != pdPASS) {
        ESP_LOGE(TAG, "client_task create failed");
        return ESP_FAIL;
    }

    s_host_inited = true;
    ESP_LOGI(TAG, "USB host stack installed -- waiting for PT-* enumeration");
    return ESP_OK;
}

/* transfers */

static void xfer_in_cb(usb_transfer_t *t)  { xSemaphoreGive((SemaphoreHandle_t)t->context); }
static void xfer_out_cb(usb_transfer_t *t) { xSemaphoreGive((SemaphoreHandle_t)t->context); }

static int usb_send(void *ctx, const uint8_t *data, size_t len)
{
    struct pt_transport_usb_host *u = ctx;
    if (!u->device_open) return -1;
    if (len > USB_BUF_SIZE) return -1;

    if (xSemaphoreTake(u->bus_lock, pdMS_TO_TICKS(5000)) != pdTRUE) return -1;

    memcpy(u->xfer_out->data_buffer, data, len);
    u->xfer_out->num_bytes        = (int)len;
    u->xfer_out->bEndpointAddress = u->ep_out_addr;
    u->xfer_out->device_handle    = u->dev_hdl;
    u->xfer_out->callback         = xfer_out_cb;
    u->xfer_out->context          = u->xfer_out_done;

    int rc = -1;
    if (usb_host_transfer_submit(u->xfer_out) == ESP_OK) {
        /* OUT transfers complete quickly -- give them a generous timeout
         * but not unbounded. If something is wrong with the bus a stall
         * here is better than a hang. */
        if (xSemaphoreTake(u->xfer_out_done, pdMS_TO_TICKS(5000)) == pdTRUE
            && u->xfer_out->status == USB_TRANSFER_STATUS_COMPLETED) {
            rc = u->xfer_out->actual_num_bytes;
        }
    }

    xSemaphoreGive(u->bus_lock);
    return rc;
}

static int usb_recv(void *ctx, uint8_t *out, size_t cap,
                    size_t *out_len, uint32_t timeout_ms)
{
    *out_len = 0;
    struct pt_transport_usb_host *u = ctx;
    if (!u->device_open) return -1;
    if (cap == 0) return 0;

    if (xSemaphoreTake(u->bus_lock, pdMS_TO_TICKS(timeout_ms + 5000)) != pdTRUE)
        return -1;

    /* IN transfers must be a multiple of MPS. Round cap up, capped at
     * the data buffer. */
    size_t aligned = ((cap + u->ep_in_mps - 1) / u->ep_in_mps) * u->ep_in_mps;
    if (aligned > USB_BUF_SIZE) aligned = USB_BUF_SIZE;

    u->xfer_in->num_bytes        = (int)aligned;
    u->xfer_in->bEndpointAddress = u->ep_in_addr;
    u->xfer_in->device_handle    = u->dev_hdl;
    u->xfer_in->callback         = xfer_in_cb;
    u->xfer_in->context          = u->xfer_in_done;

    int rc = 0;
    if (usb_host_transfer_submit(u->xfer_in) != ESP_OK) {
        rc = -1;
        goto done;
    }

    /* esp-idf v5.5: usb_transfer_t::timeout_ms isn't honoured yet, so
     * we time out at the semaphore level. If the wait expires the
     * transfer is still pending -- halt + flush + clear cancels it,
     * which causes the callback to fire so we can resync. */
    if (xSemaphoreTake(u->xfer_in_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        usb_host_endpoint_halt(u->dev_hdl, u->ep_in_addr);
        usb_host_endpoint_flush(u->dev_hdl, u->ep_in_addr);
        /* Wait for the (now-cancelled) transfer's callback. Bound it so
         * a totally hung stack doesn't wedge us. */
        xSemaphoreTake(u->xfer_in_done, pdMS_TO_TICKS(2000));
        usb_host_endpoint_clear(u->dev_hdl, u->ep_in_addr);
        /* Treat as zero-byte timeout success per pt_transport contract. */
        goto done;
    }

    if (u->xfer_in->status == USB_TRANSFER_STATUS_COMPLETED) {
        size_t got = (size_t)u->xfer_in->actual_num_bytes;
        if (got > cap) got = cap;
        memcpy(out, u->xfer_in->data_buffer, got);
        *out_len = got;
    } else {
        rc = -1;
    }

done:
    xSemaphoreGive(u->bus_lock);
    return rc;
}

/* public lifecycle */

pt_transport_usb_host_t *pt_transport_usb_host_open(uint32_t connect_timeout_ms)
{
    s_plite_seen = false;

    if (host_init_once() != ESP_OK) return NULL;

    /* Block until the singleton client task hands us a matching
     * device, or the caller's timeout expires. */
    if (xSemaphoreTake(s_pending_sem, pdMS_TO_TICKS(connect_timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "no PT-* attached within %u ms",
                 (unsigned)connect_timeout_ms);
        return NULL;
    }

    /* Consume the pending device under the lock so a racing DEV_GONE
     * can't free the handle out from under us. */
    usb_device_handle_t dev = NULL;
    uint8_t             addr = 0;
    if (xSemaphoreTake(s_pending_lock, portMAX_DELAY) == pdTRUE) {
        dev  = s_pending_dev;
        addr = s_pending_addr;
        s_pending_dev  = NULL;
        s_pending_addr = 0;
        xSemaphoreGive(s_pending_lock);
    }
    if (!dev) {
        ESP_LOGW(TAG, "device vanished between enumerate and claim");
        return NULL;
    }

    struct pt_transport_usb_host *u = calloc(1, sizeof *u);
    if (!u) {
        usb_host_device_close(s_client_hdl, dev);
        return NULL;
    }
    u->bus_lock      = xSemaphoreCreateMutex();
    u->xfer_in_done  = xSemaphoreCreateBinary();
    u->xfer_out_done = xSemaphoreCreateBinary();
    if (!u->bus_lock || !u->xfer_in_done || !u->xfer_out_done) {
        ESP_LOGE(TAG, "open: semaphore alloc failed");
        usb_host_device_close(s_client_hdl, dev);
        goto fail;
    }

    u->dev_hdl     = dev;
    u->dev_addr    = addr;
    u->device_open = true;

    if (pick_endpoints(u) != 0) {
        ESP_LOGE(TAG, "no printer-class bulk endpoints found");
        goto fail;
    }
    if (usb_host_interface_claim(s_client_hdl, dev, u->intf_num, 0) != ESP_OK) {
        ESP_LOGE(TAG, "interface_claim failed");
        goto fail;
    }
    u->interface_claimed = true;

    if (usb_host_transfer_alloc(USB_BUF_SIZE, 0, &u->xfer_in)  != ESP_OK) goto fail;
    if (usb_host_transfer_alloc(USB_BUF_SIZE, 0, &u->xfer_out) != ESP_OK) goto fail;

    s_active = u;
    ESP_LOGI(TAG, "PT-* paired: addr=%u intf=%u in=0x%02x out=0x%02x",
             u->dev_addr, u->intf_num, u->ep_in_addr, u->ep_out_addr);
    return u;

fail:
    pt_transport_usb_host_close(u);
    return NULL;
}

pt_transport_t pt_transport_usb_host_transport(pt_transport_usb_host_t *u)
{
    return (pt_transport_t){ .send = usb_send, .recv = usb_recv, .ctx = u };
}

bool pt_transport_usb_host_plite_seen(void)
{
    return s_plite_seen;
}

bool pt_transport_usb_host_alive(const pt_transport_usb_host_t *u)
{
    return u && u->device_open;
}

void pt_transport_usb_host_close(pt_transport_usb_host_t *u)
{
    if (!u) return;

    if (s_active == u) s_active = NULL;

    if (u->xfer_in)  usb_host_transfer_free(u->xfer_in);
    if (u->xfer_out) usb_host_transfer_free(u->xfer_out);

    if (u->interface_claimed && u->dev_hdl)
        usb_host_interface_release(s_client_hdl, u->dev_hdl, u->intf_num);
    if (u->dev_hdl)
        usb_host_device_close(s_client_hdl, u->dev_hdl);

    if (u->bus_lock)      vSemaphoreDelete(u->bus_lock);
    if (u->xfer_in_done)  vSemaphoreDelete(u->xfer_in_done);
    if (u->xfer_out_done) vSemaphoreDelete(u->xfer_out_done);

    free(u);
    /* NOTE: the USB host stack stays installed. usb_host_install is
     * one-shot per boot in this codebase -- see host_init_once. */
}
