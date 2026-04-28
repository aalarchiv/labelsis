/*
 * pt_transport_usb_host — esp-idf USB Host backend for pt_transport_t.
 * See pt_transport_usb_host.h for the API.
 *
 * Target-only — the host CMake build does not list this file. Compiles
 * for both ESP32-S2 and ESP32-S3 (identical USB-OTG controller).
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
};

/* Same VID, different firmware mode: when the side slider is in P-Lite
 * position the PT-* enumerates as USB Mass Storage (a virtual disk
 * containing Brother's "P-touch Editor Lite") with one of these PIDs
 * and exposes no printer-class interface — see the research notes in
 * the SDM (mode 3 = P-touch Template) and ptouch-esp32 prior art. We
 * can't drive prints in that state, but flagging it lets the SPA show
 * the user a useful "slide to E or hold the PLite button" hint
 * instead of generic "unavailable". */
static const uint16_t PT_PIDS_PLITE[] = {
    0x2064,  /* PT-P700 in P-Lite mode */
    0x2065,  /* PT-P750W in P-Lite mode */
};

/* Sticky flag set by on_client_event when a P-Lite-mode PT-* shows up
 * during the open() probe. Cleared on close() and on the next open()
 * attempt so a stale detection doesn't bleed into a later session. */
static volatile bool s_plite_seen;

/* Bulk transfers: PT-* MPS is 64 bytes per the SDM. We allocate 1 KB
 * per direction so a status read can request 64 (one MPS) and an OUT
 * write can fit a multi-row chunk. */
#define USB_BUF_SIZE 1024

struct pt_transport_usb_host {
    SemaphoreHandle_t        bus_lock;        /* serializes send/recv     */
    SemaphoreHandle_t        device_ready;    /* signaled on enumeration  */
    SemaphoreHandle_t        xfer_in_done;    /* signaled by IN callback  */
    SemaphoreHandle_t        xfer_out_done;   /* signaled by OUT callback */

    usb_host_client_handle_t client_hdl;
    usb_device_handle_t      dev_hdl;
    uint8_t                  dev_addr;
    uint8_t                  intf_num;
    uint8_t                  ep_in_addr;
    uint8_t                  ep_out_addr;
    uint16_t                 ep_in_mps;
    uint16_t                 ep_out_mps;

    usb_transfer_t          *xfer_in;
    usb_transfer_t          *xfer_out;

    TaskHandle_t             lib_task;
    TaskHandle_t             client_task;
    volatile bool            stop;

    bool                     installed;
    bool                     client_registered;
    bool                     device_open;
    bool                     interface_claimed;
};

/* ------------------------------------------------------------ tasks --- */

static void lib_task(void *arg)
{
    struct pt_transport_usb_host *u = arg;
    while (!u->stop) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(100), &flags);
    }
    vTaskDelete(NULL);
}

static void client_task(void *arg)
{
    struct pt_transport_usb_host *u = arg;
    while (!u->stop) {
        usb_host_client_handle_events(u->client_hdl, pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

/* ----------------------------------------------------- descriptor walk */

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

/* ----------------------------------------------- enumeration callback */

static void on_client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    struct pt_transport_usb_host *u = arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (u->device_open) return;  /* already paired */

        usb_device_handle_t dev = NULL;
        if (usb_host_device_open(u->client_hdl, msg->new_dev.address, &dev) != ESP_OK)
            return;

        const usb_device_desc_t *desc = NULL;
        if (usb_host_get_device_descriptor(dev, &desc) != ESP_OK || !desc) {
            usb_host_device_close(u->client_hdl, dev);
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
             * "unavailable". An attempted SCSI BOT auto-unstick (in
             * git history through commit 44c732f) sent the documented
             * magic bytes but did not actually trigger the EL→E flip
             * on real hardware — re-evaluate when we have a USB
             * packet capture from Brother's tool. */
            if (desc->idVendor == PT_VID) {
                for (size_t i = 0; i < sizeof PT_PIDS_PLITE / sizeof PT_PIDS_PLITE[0]; i++) {
                    if (desc->idProduct == PT_PIDS_PLITE[i]) {
                        ESP_LOGW(TAG, "PT-* in P-Lite mode (pid=%04x) — "
                                      "slide to E or hold PLite button 2s",
                                 desc->idProduct);
                        s_plite_seen = true;
                        break;
                    }
                }
            }
            usb_host_device_close(u->client_hdl, dev);
            return;
        }

        u->dev_hdl     = dev;
        u->dev_addr    = msg->new_dev.address;
        u->device_open = true;

        if (pick_endpoints(u) != 0) {
            ESP_LOGE(TAG, "no printer-class bulk endpoints found");
            usb_host_device_close(u->client_hdl, dev);
            u->dev_hdl     = NULL;
            u->device_open = false;
            return;
        }

        if (usb_host_interface_claim(u->client_hdl, dev, u->intf_num, 0) != ESP_OK) {
            ESP_LOGE(TAG, "interface_claim failed");
            usb_host_device_close(u->client_hdl, dev);
            u->dev_hdl     = NULL;
            u->device_open = false;
            return;
        }
        u->interface_claimed = true;

        ESP_LOGI(TAG, "PT-* paired: vid=%04x pid=%04x intf=%u in=0x%02x out=0x%02x",
                 desc->idVendor, desc->idProduct,
                 u->intf_num, u->ep_in_addr, u->ep_out_addr);
        xSemaphoreGive(u->device_ready);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        /* Device disconnected. We don't auto-recover — the caller can
         * detect via send/recv failures and re-open. */
        if (u->dev_hdl == msg->dev_gone.dev_hdl) {
            if (u->interface_claimed) {
                usb_host_interface_release(u->client_hdl, u->dev_hdl, u->intf_num);
                u->interface_claimed = false;
            }
            usb_host_device_close(u->client_hdl, u->dev_hdl);
            u->dev_hdl     = NULL;
            u->device_open = false;
        }
    }
}

/* --------------------------------------------------------- transfers */

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
        /* OUT transfers complete quickly — give them a generous timeout
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
     * transfer is still pending — halt + flush + clear cancels it,
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

/* --------------------------------------------------- public lifecycle */

pt_transport_usb_host_t *pt_transport_usb_host_open(uint32_t connect_timeout_ms)
{
    s_plite_seen = false;

    struct pt_transport_usb_host *u = calloc(1, sizeof *u);
    if (!u) return NULL;

    u->bus_lock      = xSemaphoreCreateMutex();
    u->device_ready  = xSemaphoreCreateBinary();
    u->xfer_in_done  = xSemaphoreCreateBinary();
    u->xfer_out_done = xSemaphoreCreateBinary();
    if (!u->bus_lock || !u->device_ready
        || !u->xfer_in_done || !u->xfer_out_done) goto fail;

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    if (usb_host_install(&host_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed");
        goto fail;
    }
    u->installed = true;

    if (xTaskCreate(lib_task, "pt_usb_lib", 4096, u, 5, &u->lib_task) != pdPASS)
        goto fail;

    const usb_host_client_config_t cli_cfg = {
        .is_synchronous   = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = on_client_event,
            .callback_arg          = u,
        },
    };
    if (usb_host_client_register(&cli_cfg, &u->client_hdl) != ESP_OK) {
        ESP_LOGE(TAG, "client_register failed");
        goto fail;
    }
    u->client_registered = true;

    if (xTaskCreate(client_task, "pt_usb_cli", 4096, u, 5, &u->client_task) != pdPASS)
        goto fail;

    if (xSemaphoreTake(u->device_ready, pdMS_TO_TICKS(connect_timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "no PT-* attached within %u ms",
                 (unsigned)connect_timeout_ms);
        goto fail;
    }

    if (usb_host_transfer_alloc(USB_BUF_SIZE, 0, &u->xfer_in) != ESP_OK) goto fail;
    if (usb_host_transfer_alloc(USB_BUF_SIZE, 0, &u->xfer_out) != ESP_OK) goto fail;

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

void pt_transport_usb_host_close(pt_transport_usb_host_t *u)
{
    if (!u) return;

    u->stop = true;
    /* Wake handler tasks so they observe `stop`. */
    if (u->installed) usb_host_lib_unblock();
    if (u->lib_task)    vTaskDelay(pdMS_TO_TICKS(50));
    if (u->client_task) vTaskDelay(pdMS_TO_TICKS(50));

    if (u->xfer_in)  usb_host_transfer_free(u->xfer_in);
    if (u->xfer_out) usb_host_transfer_free(u->xfer_out);

    if (u->interface_claimed && u->dev_hdl)
        usb_host_interface_release(u->client_hdl, u->dev_hdl, u->intf_num);
    if (u->device_open && u->dev_hdl)
        usb_host_device_close(u->client_hdl, u->dev_hdl);
    if (u->client_registered)
        usb_host_client_deregister(u->client_hdl);
    if (u->installed)
        usb_host_uninstall();

    if (u->bus_lock)      vSemaphoreDelete(u->bus_lock);
    if (u->device_ready)  vSemaphoreDelete(u->device_ready);
    if (u->xfer_in_done)  vSemaphoreDelete(u->xfer_in_done);
    if (u->xfer_out_done) vSemaphoreDelete(u->xfer_out_done);

    free(u);
}
