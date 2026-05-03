# Usage

How to drive the device from the SPA once it's on the LAN.

## The web UI

Open `http://labelsis.local/` (or the device IP) in a browser. Three views:

- **Label** -- the canvas designer. Add text, images, barcodes, QR
  codes, icons, rectangles. Right-click an element for context
  actions; drag the layer list to reorder. The canvas previews at
  true tape geometry, so on-screen pixels map 1:1 to printed dots.
- **Status** -- live printer status (model, tape width, tape colour,
  errors). Polls /api/status every couple of seconds. The dot in the
  title bar is green when a real printer is attached and idle, red
  otherwise.
- **Settings** -- Wi-Fi reconfig, API host (for SPA dev against a
  remote device), UI language.

The current label design and view state auto-save to `localStorage`,
so a reload picks up where you left off. The label library
(top-right) keeps named designs for quick recall.

## CSV-templated batch printing

Open the *Template & CSV data* panel under the Label view. The first
CSV row is treated as field names; subsequent rows are data. Variables
in the design refer to fields:

```
{name}                       -- field "name" from the current row
{$row}                       -- 0-based row index (skips header)
{$copy}                      -- 1-based copy number (used with print all)
{$seq|+1000}                 -- a counter with an offset
{$dt|YYYY-MM-DD}             -- current date in any custom format
{$dt|YYYY-MM-DD|+86400}      -- date offset by N seconds
\{ \}                        -- literal braces (escape)
```

Click *Print all* to run the design once per CSV row. Designs that
expand to >10 labels prompt for confirmation; the hard cap is 1000 to
prevent runaways.

## P-Lite mode

Some PT-* models have a side slider with two positions:

- **E** (normal Edit mode) -- exposes a USB printer-class interface;
  this is what the firmware drives.
- **EL** (Editor Lite / P-Lite) -- re-enumerates as USB Mass Storage
  hosting Brother's Windows-only "Editor Lite" tool. The firmware
  **cannot drive prints** in this mode.

If the slider is in EL position, the SPA shows a message:
`transport: P-Lite mode -- slide switch to E or hold the PLite
button 2 s`. Physically slide the switch or hold the printer's
PLite button for ~2 s to flip back; the ESP32's transport monitor
re-pairs the printer automatically within a few seconds.

Driving the printer out of P-Lite over USB alone is on the backlog
(`pt700-wmf`, P4). Three implementation attempts have been reverted
so far -- see [doc/RESEARCH-PLITE.md](RESEARCH-PLITE.md) for the
post-mortem and what would unblock it (a USB packet capture of
Brother's own tool flipping a real PT-P700).
