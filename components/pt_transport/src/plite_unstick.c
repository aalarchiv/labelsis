#include "plite_unstick.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

static const char *TAG = "pt_plite";

/* Mount path on the local VFS for the printer's pseudo-FAT volume.
 * Anything under here is gone the moment we unregister, so don't
 * leak file handles. */
#define PLITE_MOUNT "/plite"
#define PLITE_FILE  PLITE_MOUNT "/PTLITE.PRN"

/* The Windows tool requires PTLITE.PRN to be at least 0x200 bytes
 * (one sector). The magic always goes at fixed offset 0x1F6 within
 * the file (= last 10 bytes of sector 0), NOT at the file's tail --
 * the firmware only sniffs sector 0 of the file's first cluster.
 * Real PTLITE.PRN files seen on device are tens of KB. */
#define PLITE_MIN_SIZE  0x200
#define PLITE_MAGIC_OFF 0x1F6

/* Magic byte sequence the firmware sniffs at the tail of PTLITE.PRN.
 *
 *   1B 69 61 01    ESC i a 01    raster-mode preamble
 *   1B 69 55 66    ESC i U f     mode-switch opcode
 *   00 00          operands       EL -> E selector + reserved
 *
 * Verified against Brother's BrUsbPrnIO.dll BrEsSwitchELtoETempW
 * (PE32 i386 disassembly via Ghidra, 2026-05-02). */
static const uint8_t PLITE_UNSTICK_MAGIC[10] = {
    0x1B, 0x69, 0x61, 0x01,
    0x1B, 0x69, 0x55, 0x66,
    0x00, 0x00,
};

static bool s_msc_inited;

static void msc_event_cb(const msc_host_event_t *e, void *arg)
{
    (void)arg;
    /* The unstick worker uses msc_host_install_device(addr) directly
     * rather than waiting on this callback, so we just log for
     * debugging. The driver still needs a non-NULL callback. */
    if (e->event == MSC_DEVICE_CONNECTED) {
        ESP_LOGD(TAG, "msc connect addr=%u", e->device.address);
    } else if (e->event == MSC_DEVICE_DISCONNECTED) {
        ESP_LOGD(TAG, "msc disconnect");
    }
}

esp_err_t plite_unstick_init(void)
{
    if (s_msc_inited) return ESP_OK;

    const msc_host_driver_config_t cfg = {
        .create_backround_task = true,
        .task_priority         = 5,
        .stack_size            = 4096,
        .core_id               = tskNO_AFFINITY,
        .callback              = msc_event_cb,
        .callback_arg          = NULL,
    };
    esp_err_t err = msc_host_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install: %s", esp_err_to_name(err));
        return err;
    }
    s_msc_inited = true;
    ESP_LOGI(TAG, "MSC host installed (P-Lite unstick ready)");
    return ESP_OK;
}

/* Build the payload buffer in place. Caller-allocated `buf` of length
 * `size` (== file size); we zero it then copy magic to the FIXED
 * offset 0x1F6 (= last 10 bytes of sector 0). The firmware only
 * sniffs sector 0 of the file's first cluster -- writing the magic
 * at size-N (the file tail) lands it past the sniff window, which is
 * what an earlier attempt got wrong. */
static void build_payload(uint8_t *buf, long size)
{
    memset(buf, 0, (size_t)size);
    memcpy(buf + PLITE_MAGIC_OFF,
           PLITE_UNSTICK_MAGIC,
           sizeof PLITE_UNSTICK_MAGIC);
}

static void unstick_task(void *arg)
{
    uint8_t                   addr = (uint8_t)(uintptr_t)arg;
    msc_host_device_handle_t  dev  = NULL;
    msc_host_vfs_handle_t     vfs  = NULL;
    FILE                     *f    = NULL;
    uint8_t                  *buf  = NULL;

    ESP_LOGI(TAG, "P-Lite unstick: starting (usb addr %u)", addr);

    /* Give the device a moment to settle after enumeration before we
     * try to claim it. The Windows tool also waits implicitly via
     * SetupDi enumeration. */
    vTaskDelay(pdMS_TO_TICKS(500));

    if (msc_host_install_device(addr, &dev) != ESP_OK) {
        ESP_LOGW(TAG, "msc install_device(%u) failed", addr);
        goto cleanup;
    }

    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 2,
        .allocation_unit_size   = 0,
    };
    if (msc_host_vfs_register(dev, PLITE_MOUNT, &mount_cfg, &vfs) != ESP_OK) {
        ESP_LOGW(TAG, "msc vfs_register(%s) failed", PLITE_MOUNT);
        goto cleanup;
    }

    f = fopen(PLITE_FILE, "rb+");
    if (!f) {
        ESP_LOGW(TAG, "fopen(%s) failed: %s", PLITE_FILE, strerror(errno));
        goto cleanup;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        ESP_LOGW(TAG, "fseek(end) failed: %s", strerror(errno));
        goto cleanup;
    }
    long size = ftell(f);
    if (size < PLITE_MIN_SIZE) {
        ESP_LOGW(TAG, "PTLITE.PRN too small: %ld < %d -- abort",
                 size, PLITE_MIN_SIZE);
        goto cleanup;
    }
    ESP_LOGI(TAG, "PTLITE.PRN size = %ld bytes", size);

    buf = malloc((size_t)size);
    if (!buf) {
        ESP_LOGE(TAG, "malloc(%ld) failed", size);
        goto cleanup;
    }
    build_payload(buf, size);

    if (fseek(f, 0, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "fseek(0) failed: %s", strerror(errno));
        goto cleanup;
    }
    size_t written = fwrite(buf, 1, (size_t)size, f);
    /* fflush + close is what actually pushes the SCSI WRITE(10)
     * commands through. The OS / FatFs cache might delay the bytes
     * until close, so we MUST close cleanly before the device
     * re-enumerates. */
    fflush(f);
    fclose(f);
    f = NULL;

    if ((long)written != size) {
        ESP_LOGW(TAG, "fwrite incomplete: %zu of %ld", written, size);
        goto cleanup;
    }
    ESP_LOGI(TAG, "P-Lite unstick: magic written, awaiting re-enumeration");

cleanup:
    if (buf) free(buf);
    if (f)   fclose(f);
    if (vfs) msc_host_vfs_unregister(vfs);
    if (dev) msc_host_uninstall_device(dev);
    /* Don't uninstall the MSC driver -- keep it ready for the next
     * P-Lite event. */
    vTaskDelete(NULL);
}

esp_err_t plite_unstick_schedule(uint8_t dev_addr)
{
    if (!s_msc_inited) {
        ESP_LOGW(TAG, "schedule called but MSC driver not installed");
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t r = xTaskCreate(unstick_task, "plite_unstick", 4096,
                               (void *)(uintptr_t)dev_addr, 5, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "unstick task spawn failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
