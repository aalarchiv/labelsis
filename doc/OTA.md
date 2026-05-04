# Over-the-air firmware update

## How

The HTTP API is unauthenticated by design (see [README §Security](../README.md#security)),
so the OTA endpoint can't accept "anyone with network reach". Instead
the auth gate is **the printer's P-Lite slider**: the device only
accepts a firmware image while the slider is physically in the **EL**
position. Same threat model as the BOOT-button Wi-Fi reset, applied
to a more discoverable, user-facing control. The OTA window
coincides with "printer can't print anyway", which makes it a
natural maintenance moment.

Models without a P-Lite slider (handhelds: PT-H500, PT-E500) cannot
OTA -- re-flash via USB.

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
| `403 not_in_plite` | Slider isn't in EL when the upload starts. |
| `403 plite_ended` | Slider returned to E while the upload was in flight. Half-written image discarded. |
| `400 image_validate` | SHA256 from the image header didn't match the bytes received -- truncated upload, network corruption, or a non-IDF binary. |
| `400 wrong_project` | Image is structurally valid but its `project_name` is not "labelsis". Image is on disk but never set as boot target; next OTA overwrites it. |
| `500 ota_*` | Flash hardware error or out-of-space. Check `idf.py monitor` for the underlying esp-idf log line. |

## Recovery

If the new image somehow boots far enough to confirm itself as valid
but is then unusable (e.g., wrong board profile), Wi-Fi reset (BOOT
button held 5 s) wipes Wi-Fi creds and brings up captive-portal
onboarding -- enough to get the device addressable again so you can
OTA back. If it doesn't even get that far, re-flash via USB.
