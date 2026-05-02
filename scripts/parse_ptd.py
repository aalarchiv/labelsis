#!/usr/bin/env python3
"""
Parser for Brother PTDF (.PTD) tape geometry files.

Brother's Windows printer drivers ship a per-model BSPP*AD.PTD blob
that lists every supported tape (width, margins, media class, ...).
This parser dumps the table so you can verify our hardcoded geometry
against Brother's authoritative numbers, or extend it for new models.

Format reverse-engineered from BSPP70I3.DLL's PTD_EnumPapersEx and
BSPP70AD.PTD (PT-P700). Same format is expected for sibling models
(PT-H500 / PT-E500 / PT-P750W) and probably for PT-D / PT-E series
that ship the same Browny02 driver generation.

How to get a .PTD file:

    1. Download the printer driver installer from Brother's support
       site (e.g. "Printer Driver" entry on the model's Downloads
       page -- Windows version).
    2. Unpack with 7z. The MSI inside also unpacks with 7z.
    3. Look for a file matching BSPP*AD.PTD (or *.PTD with the
       'PTDF' magic in the first four bytes).

Usage:

    scripts/parse_ptd.py path/to/BSPP70AD.PTD
    scripts/parse_ptd.py path/to/BSPP70AD.PTD --format markdown > table.md
    scripts/parse_ptd.py path/to/BSPP70AD.PTD --format json | jq .
    scripts/parse_ptd.py path/to/BSPP70AD.PTD --raw      # full hex dump per record
    scripts/parse_ptd.py path/to/BSPP70AD.PTD --compare  # diff vs our pt_protocol table

File layout (little-endian throughout):

    0x00  4   "PTDF"             magic
    0x04  4   0x01030000         version
    0x08  4   file_size          (matches stat size)
    0x0c  4   record_count
    0x10  4   model_block_offset (= 0x20)
    0x14  4   records_offset     (= 0xfa, 250)
    0x18  4   record_size        (= 0x18d, 397)
    0x1c  4   reserved/zero

    0x20      model block (size = records_offset - model_block_offset):
        +0x00  2   model_id  (0x6730 = PT-P700)
        +0x02  100 utf-16 model name ("Brother PT-P700")
        ... remainder zero ...

    records[] of record_size bytes each, beginning at records_offset.

Per record (offsets relative to record start):

    +0x000  100  utf-16 display name ("3.5 mm", "12 mm x 2", "HS 5.2 mm", ...)
    +0x064  100  utf-16 size string in inches ("0.13\\"", "1/2\\"", ...)
    +0x0c0  2    media_class       (1 = TZe normal, 0x11/0x17 = HSe variants)
    +0x0c2  2    form_code         (per-variant ID)
    +0x0c4  2    width_tenths_mm   (e.g. 34 = 3.4mm; Brother stores
                                    raster-aligned width, not the rounded
                                    marketing name)
    +0x0c6  2    width_tenths_mm   (duplicate)
    +0x0c8  2    tape_width_dots   (180-dpi physical raster width)
    +0x0ca  2    tape_width_dots   (duplicate)
    +0x0d6  2    f_d6              (always 0x000e -- heat-curve idx?)
    +0x0d8  2    left_margin_pins  (matches our SDM table)
    +0x0da  2    right_margin_pins (matches our SDM table)
    +0x0dc  2    min_print_dots    (always 0x0384 = 900)
    +0x0de  2    max_print_dots    (10000 for TZe, 5000 for HSe)
    +0x0e0  2    f_e0              (0x28 = 40 normal, varies for x2/x3/x4)
    +0x0f8  2    f_f8              (0x0181 only for 3.5 mm row, 0x0081
                                    for the rest -- low-density flag?)

    head_pins = 128 is implicit; print_pins = head_pins - left - right.
"""

import argparse
import json
import os
import struct
import sys


PTDF_MAGIC   = b"PTDF"
PTDF_VERSION = 0x01030000


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]


def utf16(b, o, max_bytes):
    """Decode a NUL-terminated UTF-16LE string from a fixed-width slot."""
    raw = b[o:o + max_bytes]
    end = len(raw)
    for i in range(0, len(raw) - 1, 2):
        if raw[i] == 0 and raw[i + 1] == 0:
            end = i
            break
    try:
        return raw[:end].decode("utf-16-le")
    except UnicodeDecodeError:
        return repr(raw[:end])


class PtdfError(Exception):
    pass


def parse(path):
    with open(path, "rb") as f:
        data = f.read()

    if data[:4] != PTDF_MAGIC:
        raise PtdfError(f"not a PTDF file: magic={data[:4]!r}")
    version = u32(data, 0x04)
    if version != PTDF_VERSION:
        raise PtdfError(
            f"unsupported PTDF version 0x{version:08x} "
            f"(this parser knows 0x{PTDF_VERSION:08x})"
        )

    header = {
        "version":            version,
        "file_size_header":   u32(data, 0x08),
        "file_size_actual":   len(data),
        "record_count":       u32(data, 0x0c),
        "model_block_offset": u32(data, 0x10),
        "records_offset":     u32(data, 0x14),
        "record_size":        u32(data, 0x18),
    }

    body = len(data) - header["records_offset"]
    if body % header["record_size"] != 0:
        raise PtdfError(
            f"body bytes {body} not a multiple of record_size "
            f"{header['record_size']}"
        )
    if body // header["record_size"] != header["record_count"]:
        raise PtdfError(
            f"record_count {header['record_count']} disagrees with "
            f"body/{header['record_size']} = {body // header['record_size']}"
        )

    mb = data[header["model_block_offset"]:header["records_offset"]]
    model = {
        "id":   u16(mb, 0),
        "name": utf16(mb, 2, 100),
    }

    records = []
    for i in range(header["record_count"]):
        ro  = header["records_offset"] + i * header["record_size"]
        rec = data[ro:ro + header["record_size"]]
        r   = parse_record(rec)
        r["_index"]  = i
        r["_offset"] = ro
        records.append(r)

    return header, model, records


def parse_record(r):
    left  = u16(r, 0xd8)
    right = u16(r, 0xda)
    return dict(
        name              = utf16(r, 0x000, 100),
        size_str          = utf16(r, 0x064, 100),
        media_class       = u16(r, 0xc0),
        form_code         = u16(r, 0xc2),
        width_tenths_mm   = u16(r, 0xc4),
        tape_width_dots   = u16(r, 0xc8),
        head_pins         = 128,
        print_pins        = 128 - left - right,
        left_margin_pins  = left,
        right_margin_pins = right,
        f_d6              = u16(r, 0xd6),
        f_dc              = u16(r, 0xdc),
        f_de              = u16(r, 0xde),
        f_e0              = u16(r, 0xe0),
        f_f8              = u16(r, 0xf8),
        _raw              = r,
    )


# Hardcoded mirror of components/pt_protocol/src/pt_protocol.c
# pt_tape_geometry_tze() so --compare can run without the firmware
# build dir. Update both when adding rows.
PT700_TABLE = {
    # width_mm: (left, print, right, tape_width_dots)
    4:  (52,  24, 52,  24),   # 3.5 mm tape; status reports w=4
    6:  (48,  32, 48,  42),
    9:  (39,  50, 39,  64),
    12: (29,  70, 29,  84),
    18: ( 8, 112,  8, 128),
    24: ( 0, 128,  0, 170),
}


def header_lines(header, model):
    out = []
    out.append(f"version            = 0x{header['version']:08x}")
    out.append(f"file_size hdr      = {header['file_size_header']}   "
               f"(actual = {header['file_size_actual']})")
    out.append(f"record_count       = {header['record_count']}")
    out.append(f"model_block_offset = 0x{header['model_block_offset']:x}")
    out.append(f"records_offset     = 0x{header['records_offset']:x}")
    out.append(f"record_size        = 0x{header['record_size']:x}")
    out.append(f"model_id           = 0x{model['id']:04x}")
    out.append(f"model_name         = {model['name']!r}")
    return out


COLS = [
    ("#",     lambda r: str(r["_index"])),
    ("name",  lambda r: r["name"]),
    ("class", lambda r: f"0x{r['media_class']:04x}"),
    ("form",  lambda r: f"0x{r['form_code']:04x}"),
    ("w_mm",  lambda r: f"{r['width_tenths_mm']/10:.1f}"),
    ("dots",  lambda r: str(r["tape_width_dots"])),
    ("head",  lambda r: str(r["head_pins"])),
    ("print", lambda r: str(r["print_pins"])),
    ("left",  lambda r: str(r["left_margin_pins"])),
    ("right", lambda r: str(r["right_margin_pins"])),
    ("d6",    lambda r: str(r["f_d6"])),
    ("dc",    lambda r: str(r["f_dc"])),
    ("de",    lambda r: str(r["f_de"])),
    ("e0",    lambda r: str(r["f_e0"])),
    ("f8",    lambda r: str(r["f_f8"])),
]


def render_text(header, model, records):
    rows = [{c: fn(r) for c, fn in COLS} for r in records]
    widths = {c: max(len(c), max(len(row[c]) for row in rows)) for c, _ in COLS}
    out = header_lines(header, model)
    out.append("")
    out.append("  ".join(c.ljust(widths[c]) for c, _ in COLS))
    out.append("  ".join("-" * widths[c] for c, _ in COLS))
    for row in rows:
        out.append("  ".join(row[c].ljust(widths[c]) for c, _ in COLS))
    return "\n".join(out)


def render_markdown(header, model, records):
    out = [f"# {model['name']} (model_id = 0x{model['id']:04x})", ""]
    out.append(f"PTDF version 0x{header['version']:08x}, "
               f"{header['record_count']} records.")
    out.append("")
    out.append("| " + " | ".join(c for c, _ in COLS) + " |")
    out.append("|" + "|".join("---" for _ in COLS) + "|")
    for r in records:
        out.append("| " + " | ".join(fn(r) for _, fn in COLS) + " |")
    return "\n".join(out)


def render_json(header, model, records):
    clean = []
    for r in records:
        c = {k: v for k, v in r.items() if k != "_raw"}
        clean.append(c)
    return json.dumps(
        {"header": header, "model": model, "records": clean},
        indent=2,
    )


def render_raw(records):
    out = []
    for r in records:
        out.append(f"\n--- record {r['_index']:2d} '{r['name']}' @ 0x{r['_offset']:x} ---")
        raw = r["_raw"]
        for off in range(0, len(raw), 16):
            chunk = raw[off:off + 16]
            hexs  = " ".join(f"{b:02x}" for b in chunk)
            asci  = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            out.append(f"  {off:03x}: {hexs:<47}  {asci}")
    return "\n".join(out)


def render_compare(records):
    """Diff plain TZe rows against PT700_TABLE keyed by mm width.

    Skips split-print 'x N' multi-panel rows (form_code != 0x010x) and
    HSe rows (different media_class) -- their geometry is genuinely
    different from the corresponding TZe at the same nominal mm. Our
    PT700_TABLE only models the 6 TZe widths the firmware reports
    today."""
    out = ["", "Comparing TZe rows against PT700_TABLE (rounded mm width)..."]
    matched = 0
    diffs   = 0
    skipped = 0
    for r in records:
        # Plain TZe rows: media_class == 1 AND form_code in 0x0101..0x0107
        # (the split-print x N rows are 0x0117+ with the same media_class).
        if r["media_class"] != 1 or not (0x0101 <= r["form_code"] <= 0x0107):
            skipped += 1
            continue
        # Marketing-rounded mm: status byte uses 4 for 3.5 mm tape, etc.
        # Brother's stored width_tenths_mm is the raster-aligned value
        # (34 / 59 / 89 / 119 / 178 / 236), so we round to nearest mm.
        rounded = round(r["width_tenths_mm"] / 10)
        if rounded == 3:                # 3.4 mm -> SDM "4 mm" slot
            rounded = 4
        ours = PT700_TABLE.get(rounded)
        if ours is None:
            skipped += 1
            continue
        ptd = (r["left_margin_pins"], r["print_pins"],
               r["right_margin_pins"], r["tape_width_dots"])
        if ptd == ours:
            matched += 1
            out.append(f"  ok    {r['name']:<14s}  {ptd}")
        else:
            diffs += 1
            out.append(f"  DIFF  {r['name']:<14s}  ptdf={ptd}  ours={ours}")
    out.append("")
    out.append(f"matched={matched}  diffs={diffs}  skipped={skipped} "
               f"(split-print + HSe rows)")
    return "\n".join(out)


def main():
    p = argparse.ArgumentParser(
        description="Parse a Brother PTDF (.PTD) tape geometry file.",
        epilog="See the module docstring for where to obtain a .PTD file.",
    )
    p.add_argument("path", help="path to a BSPP*AD.PTD or similar file")
    p.add_argument("--format", choices=("text", "markdown", "json"),
                   default="text",
                   help="output format (default: text)")
    p.add_argument("--raw", action="store_true",
                   help="also dump per-record hex (text format only)")
    p.add_argument("--compare", action="store_true",
                   help="diff PTDF rows against the PT-P700 table baked into "
                        "components/pt_protocol/src/pt_protocol.c")
    args = p.parse_args()

    if not os.path.isfile(args.path):
        sys.stderr.write(f"parse_ptd: file not found: {args.path}\n")
        return 2

    try:
        header, model, records = parse(args.path)
    except PtdfError as e:
        sys.stderr.write(f"parse_ptd: {e}\n")
        return 1

    if args.format == "text":
        print(render_text(header, model, records))
    elif args.format == "markdown":
        print(render_markdown(header, model, records))
    elif args.format == "json":
        print(render_json(header, model, records))

    if args.raw and args.format == "text":
        print(render_raw(records))

    if args.compare:
        print(render_compare(records))

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        # Common when piping into head/less; suppress the noisy traceback.
        sys.exit(0)
