# Source this (don't execute) to pull esp-idf into the current shell.
# Usage:  source scripts/idf-env.sh
if [ ! -f "$HOME/esp/esp-idf/export.sh" ]; then
    echo "esp-idf not found at ~/esp/esp-idf — install per https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/" >&2
    return 1 2>/dev/null || exit 1
fi
. "$HOME/esp/esp-idf/export.sh"
