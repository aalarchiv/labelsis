#!/usr/bin/env bash
# Build a release archive: one OTA-ready firmware image per board
# under boards/, plus the bootloader + partition table + merged-flash
# image needed for first-time USB flashing, plus a SHA256 manifest.
#
# Usage:
#   scripts/build-release.sh                    # all boards
#   scripts/build-release.sh -b lolin_s2_mini   # one board (repeatable)
#   scripts/build-release.sh -v 1.2.3-rc1       # override version
#   scripts/build-release.sh -o /tmp/labelsis   # override output dir
#
# Output layout:
#   release/<version>/
#     labelsis-<board>-<version>.bin              <- OTA target (POST /api/ota)
#     labelsis-<board>-<version>-merged.bin       <- single-image USB flash
#     labelsis-<board>-<version>-bootloader.bin   <- bootloader-only
#     labelsis-<board>-<version>-partitions.bin   <- partition table
#     labelsis-<board>-<version>-flash.sh         <- esptool one-liner
#     SHA256SUMS
#     manifest.txt
#
# Per-board build dirs live in build-release/<board>/ so the
# user's working build/ stays untouched.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

# Sanity: ESP-IDF environment must be sourced.
if [ -z "${IDF_PATH:-}" ]; then
    echo "ERROR: IDF_PATH not set. Source ~/esp/esp-idf/export.sh first." >&2
    exit 1
fi
command -v idf.py >/dev/null || { echo "ERROR: idf.py not on PATH" >&2; exit 1; }

# CLI parsing.
boards=()
version=""
out_root="release"
while [ $# -gt 0 ]; do
    case "$1" in
        -b|--board)   boards+=("$2"); shift 2;;
        -v|--version) version="$2"; shift 2;;
        -o|--out-dir) out_root="$2"; shift 2;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0;;
        *)
            echo "unknown arg: $1" >&2
            exit 2;;
    esac
done

# Default to every board profile that carries a target file.
if [ ${#boards[@]} -eq 0 ]; then
    for d in boards/*/; do
        name="$(basename "$d")"
        [ -f "$d/target" ] && boards+=("$name")
    done
fi
if [ ${#boards[@]} -eq 0 ]; then
    echo "ERROR: no boards found under boards/*/ with a target file" >&2
    exit 1
fi

# Version comes from git describe by default. Mirror what
# CMakeLists.txt bakes into PROJECT_VER so the filename matches what
# About panel reports.
if [ -z "$version" ]; then
    version="$(git describe --always --dirty --tags 2>/dev/null || echo unknown)"
fi
# Sanitise for filenames -- git describe is shell-safe but slashes
# from branch tags would break paths.
version="${version//\//-}"

out_dir="$out_root/$version"
mkdir -p "$out_dir"

echo "==> Release: $version"
echo "==> Boards:  ${boards[*]}"
echo "==> Output:  $out_dir"
echo

# Build one board into build-release/<board>/, then collect artifacts.
build_one() {
    local board="$1"
    local target_file="boards/$board/target"
    if [ ! -f "$target_file" ]; then
        echo "  SKIP $board -- no boards/$board/target file" >&2
        return 1
    fi
    local target
    target="$(cat "$target_file")"
    local builddir="build-release/$board"
    local sdkconfig="$builddir/sdkconfig"

    echo "==> [$board / $target] building"

    # set-target on first config; subsequent runs are incremental.
    if [ ! -f "$sdkconfig" ]; then
        idf.py -B "$builddir" -D BOARD="$board" \
               -D SDKCONFIG="$sdkconfig" \
               set-target "$target"
    fi
    idf.py -B "$builddir" -D SDKCONFIG="$sdkconfig" build

    # Merged single-image bin for USB flashing convenience.
    idf.py -B "$builddir" -D SDKCONFIG="$sdkconfig" \
           merge-bin -o "$builddir/labelsis-merged.bin"

    # Stage artifacts.
    local prefix="labelsis-$board-$version"
    cp "$builddir/labelsis.bin"                      "$out_dir/$prefix.bin"
    cp "$builddir/labelsis-merged.bin"               "$out_dir/$prefix-merged.bin"
    cp "$builddir/bootloader/bootloader.bin"         "$out_dir/$prefix-bootloader.bin"
    cp "$builddir/partition_table/partition-table.bin" \
                                                     "$out_dir/$prefix-partitions.bin"

    # Per-board flash script. Uses the merged image so a single
    # esptool call covers bootloader + partitions + app at once.
    cat > "$out_dir/$prefix-flash.sh" <<EOF
#!/usr/bin/env bash
# First-time USB flash for $board ($target). Subsequent updates can
# go via the OTA endpoint with $prefix.bin.
set -e
PORT=\${1:-/dev/ttyUSB0}
echo "Flashing $prefix-merged.bin to \$PORT (target: $target)"
esptool.py --chip $target --port "\$PORT" --baud 460800 \\
    write_flash --flash_mode dio --flash_size keep 0x0 \\
    "\$(dirname "\$0")/$prefix-merged.bin"
EOF
    chmod +x "$out_dir/$prefix-flash.sh"

    # Print binary size so the operator can eyeball OTA-slot fit.
    local bytes
    bytes="$(stat -c%s "$out_dir/$prefix.bin")"
    printf "    app size: %d bytes (%.2f MB)\n" "$bytes" "$(awk "BEGIN{print $bytes/1048576}")"
}

ok=()
fail=()
for b in "${boards[@]}"; do
    if build_one "$b"; then ok+=("$b"); else fail+=("$b"); fi
done

# Manifest + checksums.
(
    cd "$out_dir"
    sha256sum *.bin > SHA256SUMS
)

cat > "$out_dir/manifest.txt" <<EOF
LabelSis release $version
Built:       $(date -u +"%Y-%m-%dT%H:%M:%SZ") (UTC)
Built by:    $(git config user.name 2>/dev/null || echo unknown)
Boards:      ${ok[*]}
Skipped:     ${fail[*]:-(none)}

For OTA (printer in P-Lite mode):
  upload labelsis-<board>-$version.bin via the SPA Status view.

For first-time USB flash:
  ./labelsis-<board>-$version-flash.sh /dev/ttyUSB0

Verify:
  sha256sum -c SHA256SUMS
EOF

echo
echo "==> Done."
echo "    OK:      ${ok[*]}"
[ ${#fail[@]} -gt 0 ] && echo "    FAILED:  ${fail[*]}" >&2
echo "    Output:  $out_dir/"
ls -lh "$out_dir/"
