#!/usr/bin/env bash
# Configure, build, and test the host-side library and unit tests.
# Runs without esp-idf — pure host toolchain.
set -euo pipefail
cd "$(dirname "$0")/.."

# If a previous build-host was created from a different source path
# (e.g. the tree was rsync'd to another machine), CMake aborts with a
# CMakeCache.txt mismatch. Nuke and reconfigure transparently in that
# case rather than asking the user to remember.
ROOT="$(pwd)"
if [ -f build-host/CMakeCache.txt ]; then
    if ! grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=${ROOT}/host\$" build-host/CMakeCache.txt; then
        echo "host-test: stale build-host cache (different source path), recreating" >&2
        rm -rf build-host
    fi
fi

cmake -S host -B build-host -G "Unix Makefiles" >/dev/null
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure
