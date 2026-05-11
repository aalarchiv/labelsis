# Over-the-air firmware update

## How

The HTTP API is unauthenticated by design (see [README §Security](../README.md#security)),
so the OTA endpoint can't accept "anyone with network reach".

LabelSis has two physical-state-proves-intent gates that open the
OTA endpoint. Either one is enough; both close on reboot.

1. **Printer in P-Lite mode** -- slider in EL position. The default
   path on a deployed device. The window naturally coincides with
   "printer cannot print", so it doubles as a maintenance moment.
2. **BOOT button override** -- hold the device's BOOT button for
   2-30 seconds and release. The OTA gate toggles open. The button
   only acts after the device is online (Wi-Fi STA up + HTTP serving)
   so a tap during boot can't pre-arm the gate. Useful on a dev
   board with no printer connected, or on a model whose printer has
   no P-Lite slider (handhelds). Hold the same button past 30 s
   without releasing and you get the Wi-Fi-reset gesture instead
   (creds wiped + reboot, regardless of online state).

While either gate is open, an OTA section appears on the SPA's
Status view.

## Steps

1. Build the new firmware on your dev box:
   ```sh
   idf.py -D BOARD=<your-board> set-target <s2|s3>
   idf.py build
   ```
   The image lands at `build/labelsis.bin`.

2. Slide the printer's switch from **E** to **EL**. The Status view
   lights up a "Firmware update" panel.

3. Pick the `labelsis.bin` file and click **install**. The panel
   walks you through every phase: **gate check**, **upload**
   (progress bar), **verifying image on device + rebooting**
   (spinner), **waiting for device to come back** (countdown +
   spinner), and finally **slot-flip confirmation** (the panel
   reads `running_slot` from `/api/ota/status` before and after,
   so a silently-rejected upload is reported instead of looking
   like success). The SPA reloads automatically once the new
   image is live.

4. Slide the switch back to **E** so printing resumes.

## CLI alternative

For headless flashing, scripted rollouts, or any case where the
SPA can't reach the device, use `scripts/labelsis-ota.py`:

```sh
scripts/labelsis-ota.py --host labelsis.local \
                        --file build/labelsis.bin
```

It pre-flights `/api/ota/status` (so a closed gate fails fast
instead of after a 1.5 MB upload), streams the image with a
progress bar, and surfaces the firmware's error tokens directly
on failure. Stdlib-only -- no `pip install` needed. Pass `--help`
for full options.

## What the device does

- Streams the request body straight into the inactive OTA slot.
- Re-checks the gate on each chunk -- if you slide back to E
  mid-upload, the partial write is aborted and never made the boot
  target. Sliding forward + back is safe.
- Verifies SHA256 (built into the image header) when the upload
  finishes.
- Verifies `project_name == "labelsis"` from the new image's
  descriptor -- a stray ESP32 binary uploaded to `/api/ota` is
  rejected, so accidental cross-project flashes can't brick the
  device.
- Switches the boot partition, sends an HTTP 200, then reboots.
- On the new image's first boot, after Wi-Fi STA gets an IP and the
  HTTP server is serving requests, calls
  `esp_ota_mark_app_valid_cancel_rollback()`. If the new image
  crashes or reboots before reaching that point, the bootloader
  rolls back to the previous slot on the next reset.

## What can fail

Every error response now includes the underlying `esp_err_t` name
in an `esp_err` field alongside the coarse `error` token, so a
deployed device without serial access can still surface the actual
flash / OTA failure mode:

```
{"ok":false,"error":"ota_write","esp_err":"ESP_ERR_FLASH_OP_TIMEOUT"}
```

| Result | Meaning |
|---|---|
| `403 gate_closed` | Neither gate is open at upload start. Slide to EL, or hold BOOT 2-30 s + release (device must be online). |
| `403 gate_closed_mid_upload` | The gate that was open dropped while the upload was in flight (slider back to E, or BOOT toggled off). Half-written image discarded; retry. |
| `400 image_validate` | SHA256 from the image header didn't match the bytes received -- truncated upload, network corruption, or a non-IDF binary. |
| `400 wrong_project` | Image is structurally valid but its `project_name` is not "labelsis". Image is on disk but never set as boot target; next OTA overwrites it. |
| `500 ota_begin` | The OTA partition is in a bad state and the firmware's auto-erase-and-retry didn't recover it. Check the `esp_err` field for the underlying cause. Try `POST /api/reboot` (see below) and retry. |
| `500 ota_write` | Flash write failed mid-upload. Check `esp_err` (e.g. `ESP_ERR_FLASH_OP_TIMEOUT`). The firmware drains the rest of the request body before responding, so the client sees a clean response instead of a connection RST. |
| `500 ota_end` / `500 set_boot` | Late-stage failure after the bytes are in flash. Rare; check the `esp_err` field. |

## Recovery ladder (Wi-Fi-only deployed devices)

For a device that's been built into a printer or otherwise inaccessible
to USB, escalate in this order:

1. **Retry the OTA**. `api_ota_upload_inner` will try once to recover
   a wedged slot via `esp_partition_erase_range + esp_ota_begin` on
   first-write failure. If the flash settles between attempts, the
   second OTA often succeeds.

2. **`POST /api/ota/erase-next`** (behind the same gate). Synchronously
   erases the inactive slot via `esp_partition_erase_range`. Use when
   the OTA upload keeps failing with `ota_write` / `ota_begin` and
   the in-handler retry didn't help. Returns `200 OK` with `{"ok":
   true, "slot": "ota_X", "size": ...}`. Takes ~5-10 s; doesn't reboot.

   ```sh
   curl -X POST http://labelsis.local/api/ota/erase-next
   ```

3. **`POST /api/reboot`** (behind the same gate). Soft reboot via
   `esp_restart()`. Resets the flash controller's transient state,
   which is the only cure for some hardware-level wedges. Returns
   `202 Accepted` with `{"ok":true,"reboot_in_ms":500}`; device comes
   back ~5 s later.

   ```sh
   curl -X POST http://labelsis.local/api/reboot
   ```

4. **Hold the BOOT button for 30+ s** (physical). Wipes Wi-Fi creds
   and reboots into AP-mode onboarding. Useful if Wi-Fi itself is
   wedged. Bypasses the device-online guard so it works even from
   a hung state.

5. **Power-cycle the printer** (physical). Cuts power to the whole
   device. Last-resort recovery; equivalent to /api/reboot but works
   when the device isn't responding to HTTP at all.

6. **USB re-flash via `idf.py flash`**. Use when nothing else works
   or you need to install a firmware version that doesn't have the
   recovery endpoints yet.

## Recovery

If the new image somehow boots far enough to confirm itself as valid

If the new image somehow boots far enough to confirm itself as valid
but is then unusable (e.g., wrong board profile), Wi-Fi reset (BOOT
button held **30 s+** without releasing) wipes Wi-Fi creds and brings
up captive-portal onboarding -- enough to get the device addressable
again so you can OTA back. The wipe gesture intentionally ignores
the device-online guard so a Wi-Fi-bricked board can still recover.
If it doesn't even get that far, re-flash via USB.
