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

3. Pick the `labelsis.bin` file and click **install**. Progress bar
   shows the upload; the device reboots into the new image and the
   SPA reloads automatically once it can reach the device again.

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

| Result | Meaning |
|---|---|
| `403 gate_closed` | Neither gate is open at upload start. Slide to EL, or hold BOOT 2-30 s + release (device must be online). |
| `403 gate_closed_mid_upload` | The gate that was open dropped while the upload was in flight (slider back to E, or BOOT toggled off). Half-written image discarded; retry. |
| `400 image_validate` | SHA256 from the image header didn't match the bytes received -- truncated upload, network corruption, or a non-IDF binary. |
| `400 wrong_project` | Image is structurally valid but its `project_name` is not "labelsis". Image is on disk but never set as boot target; next OTA overwrites it. |
| `500 ota_*` | Flash hardware error or out-of-space. Check `idf.py monitor` for the underlying esp-idf log line. |

## Recovery

If the new image somehow boots far enough to confirm itself as valid
but is then unusable (e.g., wrong board profile), Wi-Fi reset (BOOT
button held 5 s) wipes Wi-Fi creds and brings up captive-portal
onboarding -- enough to get the device addressable again so you can
OTA back. If it doesn't even get that far, re-flash via USB.
