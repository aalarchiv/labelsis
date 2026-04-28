#!/usr/bin/env bash
# Local CI: build firmware (idf.py) and run host tests (cmake/ctest).
# Exits nonzero on the first failure.
set -euo pipefail
cd "$(dirname "$0")/.."

step() { printf '\n\033[1;34m== %s ==\033[0m\n' "$*"; }

step "host tests"
./scripts/host-test.sh

step "firmware build (esp-idf)"
# shellcheck disable=SC1090
. "$HOME/esp/esp-idf/export.sh" >/dev/null
idf.py build

step "ok"
