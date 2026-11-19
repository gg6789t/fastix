#!/bin/sh
# test_host.sh - build and run host unit tests (ASan + UBSan).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
build="$root/build-host"
# test_dtb reads QEMU's device tree, which the cross build extracts.
if [ ! -f "$root/build/qemu-arm-virt.dtb" ]; then
    echo "test_host.sh: build/qemu-arm-virt.dtb is missing; run tools/build.sh first" >&2
    exit 1
fi
[ -f "$build/build.ninja" ] || cmake -G Ninja -S "$root/tests/host" -B "$build" -DCMAKE_C_COMPILER=cc >/dev/null
ninja -C "$build"
ctest --test-dir "$build" --output-on-failure "$@"
