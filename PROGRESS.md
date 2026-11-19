# Fastix Progress

## Current Phase
**Phase 4: Drivers, VFS & Storage (`devd`, `vfsd`, Unified Page Cache, `ext2`).** Steps 4A and 4B are complete and verified (2026-11-19). Next: Steps 4C–4D (`vfsd` core, unified FD table, `cpiofs`/`tmpfs`/`devfs`/`procfs`).

## Phase 4 Checklist
- [x] 4A Foundations: `libs/libos` hash table (`hashtab.{h,c}`) and 16-way sparse radix tree (`radix.{h,c}`) with 200-seed model-checked host tests (`test_hashtab`, `test_radix`), plus `libs/libdriver` (bounds-checked FDT/DTB parser tested on `build/qemu-arm-virt.dtb` + 200 fuzz seeds in `test_dtb`, MMIO barriers, DMA cache maintenance, IRQ handler wrapper, shared VirtIO-MMIO v1/v2 split-virtqueue helper). `tools/test_host.sh`: 10/10 pass under ASan+UBSan.
- [x] 4B `devd` & isolated drivers: `procd` (`devboot.c`) spawns isolated `devd` (Rank 20) with runtime DTB (`SEL4_BOOTINFO_HEADER_FDT`), Device Untypeds, `IRQControl`, `SchedControl`, `ASIDPool`, and a 4 MB RAM untyped. `devd` parses the DTB, carves MMIO frames by binary-splitting Device Untypeds, probes VirtIO-MMIO slots, and launches isolated `uart_pl011`, `rtc_pl031` (updates VDSO `CLOCK_REALTIME` downward), `virtio_rng`, `virtio_blk` (verified with `--disk` host persistence), and `fault_test` (3 consecutive mid-call crashes recovered on the same endpoint with `-EIO` to caller and 0 byte/slot leaks in `devd`). Timer decision documented in `docs/adr/0002-timer.md` (kept MCS `ticker.c` on `qemu-arm-virt` because `KernelArmExportPTMRUser` exposes `CNTP_*` globally to all EL0 processes). QEMU suite: 39/39 pass, exit 0.
- [ ] 4C `vfsd` core: mount table, path walker, per-process FD table & open file descriptions per AGENTS.md 6.4 (replacing `procd`'s temporary Phase 3 pipe/eventfd table).
- [ ] 4D Filesystems I: `cpiofs`, `tmpfs`, `devfs`, `procfs`.
- [ ] 4E Unified page cache: shared frames for `read`/`write` and `procd` file-backed `mmap` faults + `fsync`/`sync`.
- [ ] 4F `ext2`: read/write `ext2` on `virtio_blk`, reboot persistence verified across two QEMU boots, unlinked-but-open files freed on last close.

## Done Log
- 2026-11-19: Added `waitid` (exit/stop/continue reports, `WNOWAIT`, `WNOHANG`, blocking wake-up, SIGCHLD siginfo) and `rt_sigtimedwait` (pending, poll, timeout → `EAGAIN`, wake by another process, `EINTR`) to procd; new `tests/vm/test_wait.c` (2 tests, 0 leaks). Verified: 41/41 QEMU tests; 3 planted bugs (timeout not cancelled, `WNOWAIT` ignored, wrong timeout result) each failed the suite.
- 2026-11-19: Phase 4A–4B review fixes (Claude): procd and devd now share one kernel-object allocator (`libs/libos/alloc/kalloc.c`, ~150 duplicated lines removed); the sigreturn trampoline takes the seL4 syscall number and FPSR/FPCR offsets from headers with compile-time checks; the driver-restart test fails cleanly (no hang) when a restart fails (planted bug → test failed); driver READMEs; AGENTS.md note on the two driver→procd calls; `tools/test_host.sh` explains that it needs `tools/build.sh` first.
- 2026-11-19: **Phase 4A & 4B done** (`b9915a4`). Added `libos` `hashtab` + `radix` and `libdriver` (`dtb`, `mmio`, `dma`, `irq`, `virtio` v1+v2), `servers/devd/`, isolated drivers (`uart_pl011`, `rtc_pl031`, `virtio_rng`, `virtio_blk`, `fault_test`), `procd/devboot.c`, `docs/adr/0002-timer.md`, and QEMU `--disk` support. Verified with `tools/build.sh` (0 Fastix warnings), `tools/test_host.sh` (10/10), `tests/run_qemu_tests.py` (39/39, exit 0), host disk persistence check, and planted-bug checks on all 8 new tests.
- 2026-11-19: Phase 3 done (`d374e19`, cleanup `140cb42`): POSIX process lifecycle (`execve`, `vfork`, `posix_spawn_fast`, pgid/sid, creds, `MAP_SHARED`, orphan reparenting), VDSO signals with FPU preservation, MCS timers, threads + futex + robust lists, SPSC-ring pipes + `eventfd2`; 7/7 host + 34/34 QEMU tests.
- 2026-11-19: Phase 2 done: VMA/COW virtual memory, static+PIE ELF64 loader, VDSO clock/procinfo; 6/6 host + 14/14 QEMU tests.
- 2026-11-19: Phase 1 done (`b7cd478`): buddy allocator, process spawn/destroy, zero-leak test.
- 2026-11-19: Phase 0 done (`54fe7c5`): seL4 17.0.0 boots procd in QEMU.

## Next Steps
1. Step 4C: `vfsd` core (mount table, path walker with 200-seed host tests, unified FD table replacing `procd/pipe.c`'s temporary table).
2. Step 4D: `cpiofs`, `tmpfs`, `devfs`, `procfs` (move embedded programs/drivers into initramfs CPIO).
3. Steps 4E–4F: unified page cache and `ext2` on `virtio_blk` with reboot persistence.

## Known Issues & Open Questions
- `virtio_blk` moves one 512-byte sector per message (slow). Before ext2 (4F) its data path must DMA directly into `vfsd` page-cache frames (AGENTS.md 6.3 D).
- On QEMU `virt`, 512-byte VirtIO-MMIO slots pack 8 per 4 KB page (`0x0a003000..0x0a003fff`), so `virtio_rng` and `virtio_blk` share a copy of that 4 KB MMIO page cap (isolated from PL011, PL031, GIC, and all RAM/DMA).
- Driver ELFs are embedded in `devd` (`.fastix_drivers`) and `devd` is embedded in `procd` until Step 4D (`cpiofs` initramfs) loads them from disk/initramfs.
- `procd` still holds the temporary 64-slot FD table (`pipe.c`) for `pipe2`/`eventfd2` until Step 4C moves FD numbering to `vfsd` (`AGENTS.md 6.4`).
- `servers/procd/ticker.c` stays active on `qemu-arm-virt` per `docs/adr/0002-timer.md` (no safe isolated MMIO timer on `qemu-arm-virt`).
- `timerfd_*` and `signalfd4` wait for real FDs in `vfsd` (4C); `ITIMER_VIRTUAL`/`ITIMER_PROF` need CPU-time accounting (Phase 5–7).
- seL4's build still needs the `tools/build.sh` symlink workaround for the space in `my stuff`.

## Key Numbers (QEMU TCG on Apple Silicon, debug kernel, Fastix code -O2)
| Item | Measured | Budget |
| :--- | :--- | :--- |
| seL4 kernel `.text` + `.rodata` | 160,564 B | <= 250 KB |
| `procd` `.text` + `.rodata` (incl. ~85 KB embedded tests + `devd` + 5 drivers) | 157,936 B (own code ~73 KB) | <= 192 KB |
| `devd` `.text` + `.rodata` (own code; +13.4 KB `.fastix_drivers` embedded) | 17,032 B (30,432 B total) | <= 32 KB |
| Isolated drivers (`.text` + `.rodata`) | `uart_pl011` 2.3 KB, `rtc_pl031` 0.2 KB, `virtio_rng` 3.8 KB, `virtio_blk` 3.5 KB, `fault_test` 0.2 KB | <= 32 KB each |
| Full boot image (`fastix-image.elf`) | 491,496 B (480 KB) | <= 4 MB |
| Pipe 4 MiB stream (QEMU TCG, not a benchmark) | ~162 MB/s | hardware target >= 4 GB/s |
| All QEMU tests (41 tests), start to finish | ~7.2 s | test time, not boot time |
