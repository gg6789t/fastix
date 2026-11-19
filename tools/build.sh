#!/bin/sh
# build.sh - single-command Fastix build.
# Usage: tools/build.sh [build-dir]   (default: build/)
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)

# seL4's build pastes paths into compiler/linker flags unquoted, so it cannot
# build from a path containing spaces. Build through a space-free symlink.
case "$root" in *" "*)
    link="${XDG_CACHE_HOME:-$HOME/.cache}/fastix-src-$(printf '%s' "$root" | cksum | cut -d' ' -f1)"
    mkdir -p "$(dirname "$link")"
    ln -sfn "$root" "$link"
    root=$link
    ;;
esac
build=${1:-"$root/build"}
case "$root$build" in *" "*) echo "build.sh: paths must not contain spaces: $root $build" >&2; exit 1;; esac

# Host tools (macOS + Homebrew; override with LLVM_BIN / CPIO_BIN).
llvm_bin=${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}
cpio_bin=${CPIO_BIN:-/opt/homebrew/opt/cpio/bin}   # seL4 needs GNU cpio
export PATH="$llvm_bin:$cpio_bin:$root/.venv/bin:$PATH"

for tool in clang ld.lld llvm-objcopy cmake ninja qemu-system-aarch64; do
    command -v "$tool" >/dev/null || { echo "build.sh: missing host tool: $tool" >&2; exit 1; }
done
cpio --version 2>/dev/null | grep -q GNU || { echo "build.sh: GNU cpio not found" >&2; exit 1; }

if [ ! -f "$root/third_party/seL4/CMakeLists.txt" ]; then
    git -C "$root" submodule update --init
fi
if [ ! -x "$root/.venv/bin/python3" ]; then
    python3 -m venv "$root/.venv"
    "$root/.venv/bin/pip" install -q --disable-pip-version-check -r "$root/tools/requirements.txt"
fi

if [ ! -f "$build/build.ninja" ]; then
    cmake -G Ninja -S "$root" -B "$build" \
        -DCMAKE_TOOLCHAIN_FILE="$root/third_party/seL4/llvm.cmake" \
        -DTRIPLE=aarch64-none-elf \
        -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
fi
ninja -C "$build"
echo "build.sh: image at $build/fastix-image.elf"
