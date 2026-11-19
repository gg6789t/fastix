#!/bin/sh
# test_host.sh - build and run host unit tests (ASan + UBSan).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
build="$root/build-host"
[ -f "$build/build.ninja" ] || cmake -G Ninja -S "$root/tests/host" -B "$build" -DCMAKE_C_COMPILER=cc >/dev/null
ninja -C "$build"
ctest --test-dir "$build" --output-on-failure "$@"
