# Oracle harness

Build-time tools for capturing canonical PT-* command byte streams from
existing open-source implementations. Output files become the golden
fixtures that `pt_protocol` encoder tests diff against.

This directory is **never compiled into firmware**. It is only used on a
developer machine to generate fixtures committed to `test/fixtures/`.

## Tools

### `ptouch-770-capture`

A patched derivative of `refs/ptouch-770/ptouch-770-write.c` (GPL-2.0+) that
emits the same byte sequence to a regular file instead of a USB device.

```
ptouch-770-capture <input.pbm> <output.bin> <media_width_mm> [--no-compression]
```

The output file contains the literal bytes the upstream tool would have
written to `/dev/usb/lpN` for the given input. With `--no-compression`,
PackBits is disabled (`M 0x00`) and rows are emitted raw via
`g 0x10 0x00 <16 bytes>`.

Modifications vs. upstream are documented at the top of the source file.

## Building

```sh
cmake -S tools/oracle -B build-oracle
cmake --build build-oracle
```

## Capturing fixtures

```sh
./tools/oracle/capture-fixtures.sh
```

Writes `test/fixtures/<name>_<width>mm[_raw].bin` plus a sidecar
`<name>_<width>mm[_raw].json` describing the input.
