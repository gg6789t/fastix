# Fastix

**Fastix** is a small, fast, POSIX-compatible Unix operating system built on the
[seL4](https://sel4.systems) microkernel, written in C. The idea is something like QNX, but open
source: a tiny kernel, with everything else in small isolated programs called servers.

> **Status: early development.** Phases 0–3 and Phase 4A–4C (isolated drivers and the `vfsd` file
> server with an in-memory filesystem) are done. Fastix boots in QEMU and runs automated test
> suites; there is no disk filesystem, shell, or libc yet. See [`PROGRESS.md`](PROGRESS.md).

## Goals

- **Small:** a boot image of a few hundred KB today; the target is under 4 MB for the whole system.
- **Fast:** fast paths that skip IPC (the clock and `getpid` read shared pages), seL4's IPC fast
  path, copy-on-write `fork`, and shared-memory pipes.
- **POSIX:** run ordinary C programs unchanged once the musl libc port lands (Phase 5).
- **Isolated:** each server and driver runs in its own seL4 protection domain.

The full design and roadmap is in [`AGENTS.md`](AGENTS.md).

## What works now

| Phase | Area | State |
| :--- | :--- | :--- |
| 0 | Toolchain, build, seL4 boots in QEMU | Done |
| 1 | Untyped/capability allocators, isolated processes, IPC, zero-leak teardown | Done |
| 2 | Virtual memory (`mmap`, copy-on-write, demand paging), ELF + PIE loader, VDSO clock | Done |
| 3 | `execve`, `vfork`/`posix_spawn`, signals, timers, threads, futex, pipes, `eventfd` | Done |
| 4 | `devd` + isolated drivers (4A–4B), `vfsd` + `tmpfs` (4C), page cache, filesystems (4D–4F) | In progress (4A–4C done) |
| 5–10 | musl libc, shell + userland (toybox, dash), networking, SMP, x86_64, tuning | Planned |

Everything is tested automatically: 11 host unit-test programs (randomized, run under
AddressSanitizer and UndefinedBehaviorSanitizer) and 50 behavioural tests that boot Fastix
headless in QEMU.

## Architecture

```text
 user programs  (one address space each; caps only to their servers)
 ─────────────────────────────────────────────────────────────────────
 procd   process manager and seL4 root task: processes, threads, memory,
         signals, timers, futex, pipes                       [done]
 devd    device manager & supervisor + isolated drivers
         (uart_pl011, rtc_pl031, virtio_rng, virtio_blk)     [done]
 vfsd    files: FD tables, paths, mounts, tmpfs              [done]
 ttyd, netd                                                  [planned]
 ─────────────────────────────────────────────────────────────────────
 seL4 microkernel 17.0.0 (MCS, AArch64), unmodified
```

## Building and running

The host is macOS on Apple Silicon with [Homebrew](https://brew.sh). The target is AArch64 on
QEMU's `virt` machine.

```sh
brew install llvm lld cmake ninja qemu dtc cpio python
git clone --recursive https://github.com/gg6789t/fastix.git
cd fastix

tools/build.sh                                   # builds build/fastix-image.elf
tools/test_host.sh                               # host unit tests (ASan + UBSan)
cd build && python3 ../tests/run_qemu_tests.py   # boots Fastix in QEMU and runs all tests
```

`tools/build.sh` creates a Python virtual environment with seL4's build dependencies
(`tools/requirements.txt`). GNU `cpio` is required; macOS's built-in `cpio` will not work.

## Repository layout

| Path | Contents |
| :--- | :--- |
| `include/fastix/` | Shared ABI and constants (syscalls, memory layout, errno, signals, driver protocols) |
| `servers/procd/` | The process manager and root task ([README](servers/procd/README.md)) |
| `servers/devd/` | Device manager, DTB parser, and isolated driver supervisor ([README](servers/devd/README.md)) |
| `servers/vfsd/` | The file server: FD tables, path walk, mounts, tmpfs ([README](servers/vfsd/README.md)) |
| `drivers/` | Isolated hardware drivers (`uart_pl011`, `rtc_pl031`, `virtio_rng`, `virtio_blk`, `fault_test`) |
| `libs/libos/` | Small freestanding library for servers (allocators, trees, hash/radix, ELF reader, rings) |
| `libs/libdriver/` | Shared driver SDK (FDT/DTB parser, MMIO, DMA, IRQ, VirtIO-MMIO) |
| `libs/libfastix_sys/` | User-side syscall and VDSO helpers (musl builds on this later) |
| `arch/aarch64/` | CPU-specific code |
| `tests/` | Host unit tests and QEMU test programs |
| `third_party/` | seL4, seL4_tools, util_libs (pinned git submodules, unmodified) |
| `docs/adr/` | Design decision records |

## Contributing

Read [`AGENTS.md`](AGENTS.md) (the plan and rules) and [`FORMAT.md`](FORMAT.md) (code style,
tests, reports) first. Every change must build with zero warnings and keep all tests passing.

## A note on verification

seL4 is formally verified, but only in specific configurations. Fastix currently uses a debug,
MCS configuration on QEMU, which is **not** one of them, so Fastix is "built on seL4", not a
formally verified system. See [`docs/adr/0001-sel4-config.md`](docs/adr/0001-sel4-config.md).

## License

Fastix's own code is licensed under the [BSD 2-Clause License](LICENSE). Third-party code under
`third_party/` keeps its own licenses (the seL4 kernel is GPL-2.0-only).
