# PT-P700 P-Lite Mode: Research Notes

Status: **dead-end without a USB packet capture**. This document
captures three failed theories and what's actually known so far,
so future attempts don't repeat the same dead ends.

## What is P-Lite mode

The PT-P700 has a side slider with two positions:

- **E** (Edit) — the printer enumerates with USB **printer class**
  (interface class 0x07) under VID 0x04F9 / PID 0x2061. ESC/raster
  commands flow over a bulk-OUT endpoint; status comes back over
  bulk-IN. This is what LabelSis drives.
- **EL** (Editor Lite, "P-Lite") — the printer enumerates with USB
  **mass storage class** under PID 0x2064 (or 0x2065 for the
  PT-P750W). The mass-storage volume is a small FAT16 disk
  containing Brother's portable Windows tool `PtLite10.EXE` plus
  a file `PTLITE.PRN`.

In EL mode there is no printer-class interface at all, so a host that
only knows printer class (such as our firmware) cannot send raster
data. Standard Brother behaviour is for the user to launch
`PtLite10.EXE` from the printer itself; that tool calls Brother's
`BrUsbPrnIO.dll` library to drive prints.

The original goal of `pt700-wmf` was: when we detect a PT-* enumerate
in EL mode, **programmatically flip it to E mode** so the user
doesn't have to touch the slider.

## Brother's tool: `BrEsSwitchELtoETempW`

`BrUsbPrnIO.dll` exports `BrEsSwitchELtoETempW`. The agent's
disassembly showed it:

1. Builds a 10-byte buffer on the stack:
   `1B 69 61 01  1B 69 55 66  00 00`
2. Opens `<volume_root>\PTLITE.PRN` via `CreateFileW(GENERIC_READ |
   GENERIC_WRITE, OPEN_EXISTING)`
3. Calls `GetFileSize(hFile)` -- aborts if smaller than 0x200
4. Allocates a buffer of that file size, zeros it, copies the 10
   bytes to **fixed offset 0x1F6** in the buffer (last 10 bytes of
   sector 0)
5. `WriteFile(hFile, buf, fileSize)` -- writes the whole zero-filled
   buffer with magic at offset 0x1F6
6. `CloseHandle`
7. Polls for the new device: 20 iterations, `Sleep(500)` each (~10 s
   budget), via `SetupDiGetClassDevsW` + `SetupDiEnumDeviceInfo`
   matching against a hardware-ID substring like `VID_04F9&PID_2061`

So the disassembly is unambiguous about what bytes Brother writes,
where, and that the device should re-enumerate within ~10 s.

## What we tried (three failed theories)

### Theory 1: write magic via raw SCSI BOT WRITE(10) at LBA 0

Implemented in commit `85a015b` (2026-04-28). Sent a hand-crafted CBW
+ data-out + CSW sequence containing the 10 magic bytes via the
mass-storage interface, targeting LBA 0.

Hardware test: all SCSI transfers completed status=0 but the printer
never flipped. Reverted in `f57d9e3`.

Postmortem: writing to LBA 0 lands on the volume's boot sector, not
on PTLITE.PRN's data clusters. The printer firmware presumably only
acts on writes to PTLITE.PRN's actual location.

### Theory 2: write the magic via FatFs at file offset (size - 10)

Implemented in commit `86fab68` (2026-05-02), reverted shortly after.
Used `espressif/usb_host_msc` + esp-idf's FatFs to mount the volume
and `fopen(PTLITE.PRN, "rb+")`, then wrote zeros + magic at the
**file's tail** (`size - 10`).

Hardware test: bytes written successfully, printer didn't re-enumerate
in our 8-second hold. (Anecdotally re-enumerated ~77 s later in one
test, suspected to be unrelated -- maybe an internal printer timer.)

Postmortem: misread Brother's offset. The agent's report said *"at
offset (0x200 - 10)"* meaning **fixed offset 0x1F6**, not "size - 10".
For a 45 KB PTLITE.PRN, the magic landed at byte 45046 -- nowhere
near the firmware's actual sniff window.

### Theory 3: write the magic at fixed offset 0x1F6, with sync + 8 s mount-hold

Implemented in commit `d27ff27` (2026-05-03). Same approach as Theory
2 but at the correct file offset, with explicit `fsync` after the
write and the volume kept mounted for 8 s before unregister.

Hardware test added a readback diagnostic confirming the bytes
physically landed at offset 0x1F6:

    verify @ 0x1F0 (read 16 bytes):
    00 00 00 00 00 00 1b 69 61 01 1b 69 55 66 00 00

The bytes are on disk in the right place. The printer **still does
not re-enumerate**.

## What the V850 firmware actually says

A focused Ghidra dive on the FP-MAIN PD3 firmware
(`refs/extracted/pdz_109/PT-P700_FP-MAIN_ALL_USA_V0109.PD3`) reveals
the load-bearing finding that explains why none of this is working:

**The FP-MAIN firmware contains zero USB MSC code.**

Evidence:

- Strings sweep for source filenames lists only PTCBP files:
  `if_rcvtsk.c`, `if_sendtsk.c`, `if_ptcbp.c`. No `if_msc*.c`,
  `if_bot*.c`, `if_scsi*.c` etc.
- The CBW/CSW signature strings `"USBC"` (file 0x2d410) and `"USBS"`
  (file 0x2d430) exist as data only -- a Python scan for both the
  literal pointer and the in-line constant `0x43425355` finds
  **zero references in code**.
- The code+data ends at ~0x2d6b0; the rest (to 0x30000) is `0xFF`
  flash padding. There is no hidden MSC blob.

So the MSC handler that processes writes to PTLITE.PRN, and whatever
logic decides to flip out of EL mode, lives in a **separate firmware
partition not present in this dump** -- almost certainly the V850 boot
ROM or a dedicated EL-mode firmware that the FP-MAIN PDZ updater
doesn't touch.

We literally cannot reverse-engineer the trigger condition from what
we have.

## What PTLITE.PRN actually is

The reference PTLITE.PRN extracted from the disk image
(`refs/extracted/disk_106/plp700w10006e/PT-P700_Disk_V106.pdz` →
4 MB FAT16 → `PTLITE.PRN`) is **45056 bytes of repeating ASCII
`"1234567890..."`**, ending with `0D 0A`. The byte at offset 0x1F6 in
the shipped file is plain ASCII `'3'`. The 10 magic bytes appear
nowhere.

So PTLITE.PRN **is not a marker file** with a trigger pattern. It's
a **print-job spool target**: P-touch Editor Lite (running from the
same MSC drive) writes a complete PTCBP print job into it, the
printer streams those bytes through its PTCBP parser, and prints.
The 1234567890... is just placeholder content.

The bytes `1B 69 61 01 1B 69 55 66 00 00` are themselves real PTCBP
commands (`ESC i a 01` = switch to raster mode; `ESC i U f 00 00` =
in-print-mode config setter handled at FP-MAIN `0x1f154`). They are
**not a unique trigger pattern**. Brother's tool writing them at
offset 0x1F6 amounts to *starting an aborted print job*.

## So why does Brother's tool work?

This is the open question. Possibilities, none verifiable from what
we have:

1. The (missing) MSC firmware does pattern-match for that exact byte
   sequence in writes to PTLITE.PRN's first cluster, and triggers
   re-enumeration on a hit. This would make Brother's `1B 69 55 66
   00 00` an undocumented mode-switch trigger that lives only in the
   EL firmware.
2. The (missing) MSC firmware streams the file content through its
   PTCBP parser the moment a write is detected. The 502 bytes of
   leading zeros plus the two ESC commands constitute *just enough*
   of a malformed job to provoke a reset, which manifests as USB
   re-enumeration.
3. `BrEsSwitchELtoETempW` calls additional helpers we missed (extra
   SCSI commands, vendor control transfers, MS-OS descriptors) that
   actually do the work; the WriteFile is incidental decor.

We tried (3) by re-reading the disassembly carefully. Found no
additional transfers in the documented call path. But "no additional
transfers in the call path we traced" is not the same as "no
additional mechanism exists".

## What would unblock this

**A USB packet capture of Brother's `BrEsSwitchELtoETempW` actually
flipping a real PT-P700 from EL to E.**

Procedure:

1. Windows machine, Brother's printer driver installed, Brother's
   "P-touch Update Software" or "Printer Setting Tool" available.
2. Install Wireshark with USBPcap.
3. Plug in the PT-P700 in EL position; verify it shows up as PID
   0x2064 mass storage.
4. Start USBPcap on the printer's USB device.
5. Use Brother's tool to flip the printer to E (any operation that
   internally calls `BrEsSwitchELtoETempW` will do; printing from
   `PtLite10.EXE` is one path).
6. Stop the capture once the printer re-enumerates as PID 0x2061.
7. Save the .pcap to `refs/captures/` (gitignored, local-only).

Decode the capture and we'll know:

- Every byte sent on every endpoint
- Every SCSI command issued (CBW/CSW pairs)
- Whether SYNCHRONIZE_CACHE (0x35) is part of it
- Whether vendor control transfers (`bmRequestType` 0x40 / 0xC0)
  precede or follow the file write
- The exact transfer length / LBA range of every WRITE(10)
- Whether the device re-enumerates spontaneously after the writes,
  or after some specific further command

With that data, the implementation is straightforward.

## Fallback experiment if you want one

If a USB capture isn't on the table, the only experiment with
high upside on what we have is **writing a complete valid PTCBP
print job to PTLITE.PRN**. If the printer prints physical tape
with the slider in EL position, we've discovered something
genuinely useful: direct printing via the P-Lite spool, no
mode-switch needed. Same hardware test cycle, very different
outcome -- either it works (huge win) or it doesn't (confirms
we cannot drive the EL firmware from outside without the
missing pieces).

This is unimplemented as of writing. The shape would be: same
MSC + FatFs scaffolding as Theory 3, but `build_payload` produces
a real PTCBP job (invalidate + init + raster + per-row data +
page-eject) instead of zeros + magic. We have all the encoders in
`components/pt_protocol/`.

## What lives in the codebase right now

- **No auto-unstick code** -- removed in commit reverting Theory 3
  back to detect-and-hint behaviour.
- **P-Lite detection** -- `usb_host.c` still detects PIDs 0x2064 /
  0x2065 and surfaces the `transport: "plite"` state to the SPA.
- **SPA hint** -- the title-bar status shows "P-Lite mode -- slide
  switch to E or hold the PLite button 2 s" when the slider is in
  EL. The ESP32 picks up the re-enumerated printer automatically
  via the always-running transport monitor; no reboot needed.
- **`scripts/parse_ptd.py`** -- not P-Lite related, but a useful
  byproduct of the same Brother-tooling reverse-engineering session.

## bd memories worth reading before re-attempting

- `p-lite-usb-auto-unstick-attempt-failed-on` -- chronological log
  of what's been tried
- `pt-p700-fp-main-no-msc-code` -- the structural finding about the
  missing firmware partition
- `pt-p700-ptlite-prn-is-print-spool-not-marker` -- what PTLITE.PRN
  actually is
