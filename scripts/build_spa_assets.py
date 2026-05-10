#!/usr/bin/env python3
"""
Build SPA assets for embedding into the firmware.

Reads from --spa-dir, writes to --output-dir:

    index.html              original index.html with qrcode.min.js
                            inlined as a <script>...</script> block
                            and the LabelSis app icon (--icon-path)
                            inlined as a base64 data: URL into the
                            __LABELSIS_ICON_DATA_URL__ placeholder
                            (saves an HTTP round-trip + a separate
                            embed; lets both ride the SPA gzip pass)
    index.html.gz           gzip(index.html) at level 9
    setup.html              unchanged copy
    setup.html.gz           gzip(setup.html)
    material-icons.json     name -> codepoint table; values converted
                            from upstream's hex strings ("eb8d") to
                            decimal integers (60301) so the SPA can
                            do String.fromCodePoint(cp) without parsing
    material-icons.json.gz  gzip(material-icons.json)

The plain copies are kept so the HTTP server can fall back when a
client doesn't send Accept-Encoding: gzip (rare but possible). The
script is the *only* place that produces both forms, so the gzipped
and ungzipped copies stay in lockstep -- there is no codepath where
one can be stale relative to the other. CMake invokes this with
proper DEPENDS so source changes always trigger a rebuild.

Reproducibility: gzip mtime is forced to 0 so identical sources
produce identical outputs (binary diffs only when content changes).

Usage:
    scripts/build_spa_assets.py \\
        --spa-dir    components/pt_app/spa \\
        --icon-path  doc/labelsis_favicon_64.png \\
        --output-dir build/spa-bundle
"""

import argparse
import base64
import gzip
import json
import os
import re
import sys


# Match exactly the reference the firmware build expects to inline.
# We accept either bare or quoted attribute and either "/qrcode.min.js"
# (root path) or "qrcode.min.js" (relative). The replacement is a
# single inline <script>...</script> block.
QRCODE_TAG_RE = re.compile(
    r'<script\s+src=["\']/?qrcode\.min\.js["\']\s*></script>'
)


def inline_qrcode(html_text, qrcode_text):
    """Replace the <script src='qrcode.min.js'></script> tag with an
    inline <script>...</script> block containing the JS verbatim.
    Errors out unless exactly one matching tag is present -- the
    bundler must be deterministic and not silently no-op."""
    matches = QRCODE_TAG_RE.findall(html_text)
    if len(matches) != 1:
        raise RuntimeError(
            f"expected exactly 1 <script src='qrcode.min.js'> tag in "
            f"index.html, found {len(matches)}"
        )
    inline = "<script>\n" + qrcode_text + "\n</script>"
    return QRCODE_TAG_RE.sub(lambda _: inline, html_text, count=1)


# Single placeholder string emitted by index.html in src="..."; we
# replace it with the data: URL of the icon PNG. Programmatic
# substitution (vs hand-pasted base64) is the entire point: a manual
# copy-paste of multi-KB base64 corrupted three chars before -- the
# decoded PNG was structurally valid but visibly broken in browsers.
ICON_PLACEHOLDER = "__LABELSIS_ICON_DATA_URL__"


def inline_icon(html_text, icon_bytes):
    """Replace ICON_PLACEHOLDER with a data:image/png;base64,... URL
    built from icon_bytes. Errors out unless exactly one occurrence
    is present so a typo / drift can't silently no-op."""
    n = html_text.count(ICON_PLACEHOLDER)
    if n != 1:
        raise RuntimeError(
            f"expected exactly 1 {ICON_PLACEHOLDER!r} in index.html, "
            f"found {n}"
        )
    data_url = "data:image/png;base64," + base64.b64encode(icon_bytes).decode("ascii")
    return html_text.replace(ICON_PLACEHOLDER, data_url, 1)


def write_pair(out_dir, name, body_bytes):
    """Write out_dir/name and out_dir/name.gz from the same bytes."""
    plain_path = os.path.join(out_dir, name)
    gz_path    = plain_path + ".gz"
    with open(plain_path, "wb") as f:
        f.write(body_bytes)
    # mtime=0 -> deterministic output; level=9 -> small gain over 6 in
    # exchange for slow build, fine because we only do 3 files.
    with open(gz_path, "wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw,
                           mtime=0, compresslevel=9) as g:
            g.write(body_bytes)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--spa-dir",    required=True)
    p.add_argument("--icon-path",  required=True,
                   help="PNG file inlined as a data: URL into the SPA's "
                        "About panel icon placeholder")
    p.add_argument("--output-dir", required=True)
    args = p.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    def read_text(*parts):
        with open(os.path.join(args.spa_dir, *parts), encoding="utf-8") as f:
            return f.read()

    index_html  = read_text("index.html")
    qrcode_js   = read_text("qrcode.min.js")
    setup_html  = read_text("setup.html")
    # Material Icons ships codepoints as hex strings ("eb8d"). Convert
    # to decimal integers here so the SPA can pass the value straight
    # into String.fromCodePoint(cp) at render time.
    icons_raw   = json.loads(read_text("fonts", "material-icons-codepoints.json"))
    icons_dec   = {name: int(hex_str, 16) for name, hex_str in icons_raw.items()}
    # sort_keys for stable byte-for-byte builds; separators trim whitespace.
    icons_json  = json.dumps(icons_dec, sort_keys=True, separators=(",", ":"))

    with open(args.icon_path, "rb") as f:
        icon_bytes = f.read()

    bundled = inline_qrcode(index_html, qrcode_js)
    bundled = inline_icon(bundled, icon_bytes)
    write_pair(args.output_dir, "index.html",          bundled.encode("utf-8"))
    write_pair(args.output_dir, "setup.html",          setup_html.encode("utf-8"))
    write_pair(args.output_dir, "material-icons.json", icons_json.encode("utf-8"))

    # Print byte counts so the build log makes the win obvious.
    sys.stderr.write("SPA bundle:\n")
    for name in ("index.html", "setup.html", "material-icons.json"):
        plain = os.path.getsize(os.path.join(args.output_dir, name))
        gz    = os.path.getsize(os.path.join(args.output_dir, name + ".gz"))
        ratio = (gz / plain * 100) if plain else 0
        sys.stderr.write(
            f"  {name:25s} {plain:7d}  -> gz {gz:7d}  ({ratio:5.1f}%)\n"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
