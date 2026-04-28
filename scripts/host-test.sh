#!/usr/bin/env bash
# Configure, build, and test the host-side library and unit tests.
# Runs without esp-idf — pure host toolchain.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S host -B build-host -G "Unix Makefiles" >/dev/null
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure
