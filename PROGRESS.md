# Fastix Progress

## Current Phase
**Phase 3: POSIX Process Lifecycle, Signals, Threads, Futex & Pipes.** All exit criteria met (2026-11-19). Next: Phase 4 (`devd`, isolated drivers, `vfsd`, unified page cache, `ext2`).

## Phase 3 Checklist
- [x] Host tests, ASan+UBSan, 200 seeds each (`tools/test_host.sh`, 7/7 pass): new `test_ring` (SPSC ring buffer) plus earlier `slot_alloc`, `ut_buddy`, `elf`, `slab`, `rtree`, `vdso`. Planted `atomic_all` bug in `ring.c` caught on seed 3.
- [x] 3A Process lifecycle: `execve` (ELF + `#!` scripts up to depth 4, `ELOOP` past depth 4), `vfork` (shared address space + nested + `vfork`+`execve`), `posix_spawn_fast`, process groups & sessions (`setpgid`/`getpgid`/`setsid`/`getsid`, `wait4` on pgid `0`/`-pgid`), credentials (`uid`/`gid`/`resuid`/`resgid`/`umask`, synced to `procinfo`), `MAP_SHARED` anonymous memory shared across `fork` (including lazy pages), orphan reparenting to PID 1.
- [x] 3B Signals & timers: `rt_sigaction`/`rt_sigprocmask`/`rt_sigpending`/`rt_sigsuspend`/`rt_sigreturn`/`sigaltstack`/`kill`/`tkill`/`tgkill`/`rt_sigqueueinfo`; VDSO code page (`AT_SYSINFO_EHDR`) with `__fastix_sigenter`/`__fastix_sigreturn` preserving all GP and 32×128-bit FPU/SIMD registers across nested handlers on `sigaltstack`; `EINTR` vs `SA_RESTART` (`wait4` restarts, `nanosleep` returns `-EINTR` with `rem`); `SIGSTOP`/`SIGCONT` + `WUNTRACED`/`WCONTINUED` + `SIGCHLD`/`SA_NOCLDWAIT`; realtime FIFO signal queuing; `nanosleep`, `clock_nanosleep(TIMER_ABSTIME)`, `setitimer`/`getitimer`, `timer_create`/`settime`/`gettime`/`getoverrun`/`delete`.
- [x] 3C Threads & futex: `thread_create` (2-level CSpace `THREAD_CNODE`, per-thread TCB/SC/IPC buffer/`TPIDR_EL0` TLS/`TPIDRRO_EL0` slot), `set_tid_address`/`clear_child_tid`, `set_tls`, `exit` vs `exit_group`, multithreaded `fork` keeping only the calling thread; `futex` `WAIT`/`WAKE`/`REQUEUE`/`CMP_REQUEUE`/`WAIT_BITSET`/`WAKE_BITSET` (private & shared keys, timeouts) and robust lists (`OWNER_DIED` on thread exit).
- [x] 3D Pipes & eventfd: lock-free SPSC ring (`include/fastix/ring.h`, `libs/libos/ring/ring.c`), `pipe2` (`PIPE_BUF` 4 KB atomicity, EOF, `EPIPE`+`SIGPIPE`, `O_NONBLOCK`, `O_CLOEXEC`, `dup`/`dup3`/`fcntl`), `eventfd2` (normal, blocking, `EFD_SEMAPHORE`) on a clearly marked temporary Phase 3 FD table in `procd`.
- [x] Phase 3 exit criteria verified in QEMU (`tests/run_qemu_tests.py`, 34/34 pass, exit 0): 1,000× `fork+exit+waitpid` with 0 leaks; nested signals + `sigaltstack` + FPU preservation; 8-thread 100,000-op futex mutex contention; 4 MiB pipe stream + 4-writer atomic 4 KB stress; `SIGCHLD` + zombie reaping + orphan reparenting to PID 1. Planted bugs in 3A (`VMA_SHARED`), 3B (FPU restore), 3C (robust `OWNER_DIED`), and 3D (`PIPE_BUF` atomicity + `ring.c`) all caught.

## Done Log
- 2026-11-19: Phase 3 code written by `agy` (Gemini CLI) and reviewed by Claude: clean rebuild + 7/7 host + 34/34 QEMU re-run; user-memory access, signal frames/`sigreturn` (seL4 forces user-mode SPSR), futex keys and robust-list bound, exec buffers checked; planted bugs: futex wake (suite hangs → timeout), FPU restore (caught), pipe atomicity (caught only by the non-blocking test, see Known Issues).
- 2026-11-19: **Phase 3 done.** Full POSIX process lifecycle, VDSO signal trampoline with FPU preservation, MCS-driven timers, threads + futex + robust lists, and SPSC-ring pipes + `eventfd2`. Verified with clean `tools/build.sh` (0 Fastix warnings), `tools/test_host.sh` (7/7), and `tests/run_qemu_tests.py` (34/34, exit 0).
- 2026-11-19: Phase 2 done: VMA/COW virtual memory, static+PIE ELF64 loader, VDSO clock/procinfo, 14/14 QEMU + 6/6 host tests.
- 2026-11-19: Phase 1 done (`b7cd478`): buddy allocator, process spawn/destroy, zero-leak test.
- 2026-11-19: Phase 0 done (`54fe7c5`): seL4 17.0.0 boots procd in QEMU.

## Next Steps
1. Phase 4: `libs/libos` hash table + radix tree (with 200-seed host tests) and `libs/libdriver`.
2. Phase 4: `devd` + isolated `uart_pl011`, `timer`/`rtc_pl031`, `virtio_rng`, `virtio_blk` drivers.
3. Phase 4: `vfsd` (unified FD table per AGENTS.md 6.4 replacing procd's temporary Phase 3 table, page cache, `cpiofs`/`tmpfs`/`devfs`/`procfs`/`ext2`).

## Known Issues & Open Questions
- Review findings (Phase 3):
  - `arch/aarch64/ticker.S` hard-codes seL4 syscall numbers (`-5` Send, `-11` Yield). They are correct for this config but must come from libsel4 (they differ between seL4 configs).
  - `exec.c` limits: 256 argv / 256 envp entries and 32 KB of strings (below `FASTIX_ARG_MAX` = 128 KB). These constants are local to `exec.c`, not in `include/fastix/config.h`.
  - The blocking 4-writer pipe stress test cannot detect torn writes on one CPU (procd resumes parked writers in order). `PIPE_BUF` atomicity is enforced by the non-blocking `EAGAIN` check. Reader now uses uneven reads.
  - A broken futex wake makes the suite hang until the runner's timeout instead of failing one test (no per-test timeouts yet).
  - Files over the 800-line target (FORMAT.md / AGENTS.md 2.1): `proc.c` 1,102, `signal.c` 967, `vmspace.c` 892. Split them in a later cleanup.
- `procd` currently holds a temporary 64-slot FD table (`pipe.c`) for `pipe2`/`eventfd2` only; moves to `vfsd` in Phase 4 (`AGENTS.md 6.4`).
- `execve` and `posix_spawn_fast` currently resolve paths against embedded images/scripts in `images.c` until `vfsd` arrives in Phase 4.
- `timerfd_*`, `signalfd4`, `waitid`, `rt_sigtimedwait`, and `ITIMER_VIRTUAL`/`ITIMER_PROF` are not yet wired (can be added alongside `vfsd`/Phase 5–7 event broker).
- `setitimer` / `timer_*` / `nanosleep` use a 1 ms MCS periodic ticker TCB inside `procd` (suspended when idle) until the isolated Phase 4 `timer` driver takes over.
- Frames and heap pages are pooled and never returned to the untyped allocator; page tables stay with an address space until exit; `procd` still scans its 4096-entry table on exit/wait.
- seL4's build still needs the `tools/build.sh` symlink workaround for the space in `my stuff`.

## Key Numbers (QEMU TCG on Apple Silicon, debug kernel, Fastix code -O2)
| Item | Measured | Budget |
| :--- | :--- | :--- |
| seL4 kernel `.text` + `.rodata` | 160,564 B | <= 250 KB |
| `procd` `.text` (own code) | 65,384 B | <= 192 KB (text+rodata) |
| `procd` `.rodata` (incl. ~47.4 KB embedded test programs) | 52,584 B | (included above; total 117,968 B) |
| Full boot image (`fastix-image.elf`) | 443,368 B | <= 4 MB |
| Pipe 4 MiB stream (QEMU TCG, not a benchmark) | ~145 MB/s | hardware target >= 4 GB/s |
| All Phase 2+3 QEMU tests (34 tests), start to finish | ~5.81 s | test time, not boot time |
