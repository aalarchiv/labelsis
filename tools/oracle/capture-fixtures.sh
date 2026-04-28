#!/usr/bin/env bash
# Build the oracle tool and use it to capture a curated set of golden
# byte streams into test/fixtures/. Each fixture has a sidecar .json
# describing its input parameters so the encoder tests can drive the
# same inputs through pt_protocol and diff.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# Build the capture tool.
cmake -S tools/oracle -B build-oracle -G "Unix Makefiles" >/dev/null
cmake --build build-oracle -j >/dev/null
TOOL="$ROOT/build-oracle/ptouch-770-capture"

OUT="$ROOT/test/fixtures"
mkdir -p "$OUT"
PBM="$ROOT/refs/ptouch-770"

# Curated fixtures. Each row: <pbm-basename> <width-mm> <suffix> <flags...>
fixtures=(
  "tux-128px-bw           24 24mm           "
  "tux-128px-bw           24 24mm_raw       --no-compression"
  "calibrate_12mm_tape    12 12mm           "
  "calibrate_12mm_tape    12 12mm_raw       --no-compression"
  "gunda_18mm             18 18mm           "
  "l                      24 24mm_long      "
)

for spec in "${fixtures[@]}"; do
  # shellcheck disable=SC2086
  set -- $spec
  name="$1"; width="$2"; suffix="$3"; shift 3
  flags="$*"
  pbm="$PBM/$name.pbm"
  bin="$OUT/${name}_${suffix}.bin"
  json="$OUT/${name}_${suffix}.json"
  [ -f "$pbm" ] || { echo "missing pbm: $pbm" >&2; exit 1; }
  # shellcheck disable=SC2086
  "$TOOL" "$pbm" "$bin" "$width" $flags
  # Sidecar metadata: input pbm, width, compression mode, byte count.
  size=$(stat -c '%s' "$bin")
  compress=true
  [ "$flags" = "--no-compression" ] && compress=false
  cat > "$json" <<EOF
{
  "source_tool": "ptouch-770-capture",
  "source_pbm": "refs/ptouch-770/${name}.pbm",
  "media_width_mm": ${width},
  "compression": ${compress},
  "output_bytes": ${size}
}
EOF
  printf '  captured %s (%d bytes)\n' "$(basename "$bin")" "$size"
done

echo "ok: $(ls "$OUT"/*.bin | wc -l) fixtures in $OUT"
