# LabelSis v0.8.3

OTA hardening release. The headline goal: make Wi-Fi-only updates
rock-solid for a device that's been built into a printer, with no
ongoing physical access. Three audit rounds of the OTA path landed
the fixes below.

## Why upgrade

v0.8.2's OTA endpoint had a class of bugs that could *silently
wedge* the inactive flash slot: an aborted upload (RST mid-stream,
connection drop, etc.) left the partition in a state where the next
`esp_ota_begin` succeeded but the subsequent `esp_ota_write` failed
on the first byte. The error response from the firmware got lost in
the connection close, so the client just saw "network error" with
no actionable diagnostic. The only recovery was physical
power-cycle.

v0.8.3 closes that recovery gap: aborted uploads auto-recover within
one retry, the underlying ESP-IDF error code now reaches the client
in the response body, and three new HTTP endpoints (`/api/reboot`,
`/api/ota/erase-next`, plus the existing `/api/ota`) give a
complete remote recovery ladder.

## Recovery ladder (Wi-Fi-only)

For a device that's no longer USB-accessible, escalate in order:

1. **Retry the OTA** — `api_ota_upload_inner` now auto-recovers a
   wedged slot on first-write failure via
   `esp_partition_erase_range + retry`.
2. **`POST /api/ota/erase-next`** — explicit erase of the inactive
   slot, behind the same gate. Takes ~5–10 s, no reboot.
3. **`POST /api/reboot`** — soft reboot via `esp_restart()`. Resets
   the flash controller's transient state.
4. **Hold BOOT button 30+ s** — wipe Wi-Fi creds + reboot into
   AP-mode onboarding. Bypasses the device-online guard so it works
   even when HTTP is hung.
5. **Power-cycle** — equivalent to /api/reboot when HTTP is dead.
6. **USB re-flash** — last resort.

## What changed

### Firmware (OTA path)

- **Drain body before responding on error** — the client now reliably
  reads the error JSON instead of getting RST mid-stream.
- **`esp_err` in every error response** — surfaces the underlying
  `ESP_ERR_FLASH_OP_TIMEOUT` etc. instead of just the coarse token
  (`ota_write`). Diagnosis without serial access is now possible.
- **`Connection: close` on every `/api/ota` response** — clean FINs,
  no stale-keep-alive client confusion.
- **Auto-retry on `esp_ota_begin` failure** — `esp_partition_erase_range`
  followed by a fresh begin, before surrendering to the client.
- **First-write recovery** — same retry path triggered on first
  `esp_ota_write` failure (the classic "slot wedged after aborted
  prior OTA" symptom).
- **Concurrent-OTA guard** with **5-min auto-clear** — `s_ota_busy`
  atomic flag with timestamp. A handler that panics without clearing
  no longer bricks OTA permanently.
- **Content-Length guard** — `413 too_large` upfront when the claim
  exceeds the slot size. No wasted flash erase cycles.
- **Early `project_name` check** — first chunk's `esp_app_desc_t`
  validated against `magic_word` + `"labelsis"` before committing
  to the rest of the upload. A wrong-project binary fails in <1 s
  instead of after the full 20 s upload.
- **Defensive `return;` after `vTaskDelete(NULL)`** in
  `reset_button_task` — prevents a theoretical garbage-GPIO read
  from spuriously triggering the 30 s wipe gesture.

### Firmware (new endpoints)

- **`POST /api/reboot`** — soft reboot, same gate as `/api/ota`.
- **`POST /api/ota/erase-next`** — explicit slot erase, same gate.
- **`/api/ota/status`** now returns `next_slot_state`
  (`new` / `pending_verify` / `valid` / `invalid` / `aborted` /
  `undefined`). Tells the SPA / CLI whether the slot is healthy
  before wasting an upload.

### SPA

- **Spinner during the post-upload silent gap** — `verifying image
  on device + rebooting…` with a CSS spinner. No more "frozen" UI
  during the SHA verify + reboot window.
- **Slot-flip confirmation after reboot** — the unambiguous "install
  took" signal. Surfaces an explicit "install was rejected — check
  serial logs" message when the slot doesn't flip, instead of a
  misleading "device is back".
- **Pre-flight gate check with AbortController timeout** — 5 s cap
  before failing with "device unresponsive", so a stuck device
  doesn't freeze the panel.
- **`xhr.onerror` parses response body** — surfaces the error token
  (and `esp_err`) immediately when the response made it through but
  the FIN was lost.
- **`refreshStatus` pauses + aborts in-flight fetches during OTA** —
  fixes the Firefox socket-starvation that yesterday's HAR caught
  causing 1.5 MB POSTs to never reach the wire.

### SPA (other)

- **Hover help (`title=` tooltips)** on print options (auto cut,
  mirror, chain) and canvas options (length, origin, rulers, grid,
  center, colors). Click-and-hover surfaces explanations for the
  not-obvious ones.
- **`auto-cut` defaults to off** — the ~25 mm leader Brother feeds
  after each cut was an annoying surprise. Saved-session restore
  still honours whatever the user had previously.
- **LabelSis app icon in the About panel** (top-right, clickable
  → homepage). Inlined at build time from `doc/labelsis_favicon_64.png`
  via `scripts/build_spa_assets.py` — no manual base64 paste, no
  corruption risk.

### CLI (`scripts/labelsis-ota.py`)

- **Raw-socket upload** matching curl byte-for-byte on the wire.
  Bypasses an `http.client` quirk that intermittently hung on slow
  Wi-Fi.
- **Slot-flip detection** — confirms the install via `running_slot`
  change rather than version-string equality (unreliable when
  rebuilding from the same git revision).
- **`esp_err` surfaced** in failure messages.
- **Better post-upload waiting** — explicit "verifying image +
  response (≤30 s)…" instead of silent gap.

### Scripts

- **`build-release.sh -n / --notes <file>`** — bundles a markdown
  file as `RELEASE.md` in the release archive.

### Docs

- `doc/OTA.md` documents the new error-response schema, the
  `/api/reboot` and `/api/ota/erase-next` endpoints, and the
  six-step recovery ladder above.
- `README.md` highlights the multi-AP / OTA / BOOT-button features
  introduced in v0.8.x.
- `doc/TROUBLESHOOTING.md` gained OTA-specific rows for the
  install-rejected and no-clean-response cases.

## Safety net (unchanged but verified)

- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` — bad image auto-rolls
  back on first reset without `mark_app_valid_cancel_rollback`.
- `mark_app_valid` placement at the end of `http_up()` — only
  marks the new image as good once HTTP is genuinely serving.
  An image that crashes before this point rolls back. An image
  that lands in AP-mode onboarding does NOT mark valid — so a
  bad-Wi-Fi-creds new image rolls back to the working old one.

## Upgrade procedure

Standard OTA from v0.8.2:

```sh
# device in P-Lite mode (slider to EL)
scripts/labelsis-ota.py --host labelsis.local \
    --file release/v0.8.3/labelsis-<board>-v0.8.3.bin
```

Or via the SPA's `Firmware update` panel.

If your v0.8.2 device is in the "wedged slot" state from prior
experiments, USB-flash this image once to break the catch-22 —
future updates self-heal.

## Known limitations

- Some flash-hardware-level wedges may still require a power-cycle;
  the in-handler retry covers the common firmware-state class but
  cannot recover from genuine silent erase failures.
- `CONFIG_ESP_TASK_WDT_PANIC` is intentionally off — a hung task
  logs but doesn't reboot. Revisit if hardware shows hangs in the
  wild.
- The audit-round fixes are *hardware-verified only on the upgrade
  path* (uploading v0.8.3 to a v0.8.2 device verifies that part of
  the system); some of the recovery code paths haven't been
  exercised on real hardware yet because we couldn't trigger the
  specific failure modes safely.
