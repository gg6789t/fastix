# Fastix — Master Architecture, Engineering & Implementation Plan

> **Document Purpose:** This `AGENTS.md` is the authoritative engineering specification, architecture blueprint, coding standard, and phased execution plan for **Fastix**. Every engineer and AI agent working in this repository MUST read and follow this document. It covers every detail from bit-level ABI and capability-slot assignments up to the full POSIX system architecture, performance engineering, verification, and self-hosting roadmap.

---

## 1. Goal, Vision & Hard Engineering Requirements

### 1.1 Goal Statement
Build **Fastix**, a tiny, ultra-fast, lightweight, POSIX-complete Unix operating system built on the formally verified **seL4** microkernel. Fastix combines an unmodified seL4 kernel, a minimal set of high-speed isolated C OS servers, a small fast POSIX-compliant C standard library capable of compiling and running arbitrary standard C programs, and a lean optimized C userland.

### 1.2 Explicit Goals
1. **seL4 Kernel Foundation:** Run directly on upstream seL4 (preserving kernel minimality and formal verification guarantees; never bloat or fork core kernel semantics).
2. **Tiny & Lightweight:** Minimal RAM footprint, minimal root image size, zero unnecessary daemons, zero heavy runtime dependencies.
3. **Extremely Fast:** Rival or beat monolithic Unix systems on common hot paths via zero-IPC VDSO calls, seL4 fastpath `ReplyRecv`, zero-copy page-cache frame sharing, lock-free shared-memory ring buffers, userspace futex fastpaths, and `posix_spawn`/`vfork` acceleration.
4. **Full POSIX Support:** Full POSIX.1-2017 (and POSIX.1-2024 / Issue 8 alignment) system call, threading, signal, terminal/PTY, filesystem, IPC, socket, and shell/utility compliance.
5. **Small, Fast C Userland & Universal C Program Support:** A small, proven, permissively licensed C userland (`toybox` + `dash` + `onetrue-awk`, Section 8) paired with a small, fast, standards-complete libc so virtually any portable C/POSIX program compiles and runs unmodified. Fastix's own engineering effort goes into the OS (seL4 servers + libc port), not into rewriting `ls`.

### 1.3 Unstated Mandatory Engineering Requirements
Every component MUST also satisfy these baseline senior-engineering requirements:
- **Zero Resource Leaks:** Exiting or killing any process (normally, by signal, or on unhandled fault) MUST reclaim 100% of its seL4 capabilities, untyped allocations, physical frames, VMA entries, open file descriptors, locks, and IPC endpoints. Verified via leak-check tests.
- **Fault Isolation & Clean Recovery:** A crashing user program or isolated device driver MUST NOT crash `procd`, `vfsd`, or the system. Clean reboot (`reboot`), shutdown (`poweroff`), and persistent filesystem state across restarts MUST work reliably.
- **Single Source of Truth:** Every ABI number, capability slot index, virtual address boundary, buffer size, lock rank, errno value, and configuration limit MUST be defined in ONE canonical header under `include/fastix/`. No duplicated magic numbers anywhere.
- **Strict Build Hygiene:** Zero compiler warnings under strict warning flags; deterministic, reproducible builds; clean separation of architecture-specific code (`arch/`) from portable logic.
- **Honest, Behavior-Driven Verification:** Passing compilation or "not crashing" is never sufficient. Every subsystem MUST have automated behavioral tests, randomized stress tests (100+ seeds), and POSIX conformance tests run headlessly on QEMU.
- **Graceful Degradation Under Pressure:** Out-of-memory (`ENOMEM`), out-of-disk (`ENOSPC`), FD exhaustion (`EMFILE`/`ENFILE`), PID exhaustion (`EAGAIN`), and invalid user pointers (`EFAULT`) MUST return exact POSIX errors without panicking any server.

### 1.4 Quantitative Budgets & Targets

| Metric | Target Budget (AArch64 Release) | Stretch Target | Measurement Method |
| :--- | :--- | :--- | :--- |
| **seL4 kernel binary** | <= 250 KB | <= 180 KB | `size` on `kernel.elf` |
| **Core OS servers total (`procd`+`vfsd`+`devd`+`ttyd`+`netd`)** | <= 512 KB total `.text+.rodata` | <= 350 KB | `size` across server ELFs |
| **Minimal static C `hello` binary** | <= 24 KB | <= 16 KB | `ls -l` stripped static ELF |
| **Shared `libc.so` + dynamic linker (`ld-musl-<arch>-fastix.so.1`)** | <= 700 KB | <= 600 KB | `ls -l` stripped `.so` |
| **Base userland (`toybox` with Fastix config, static)** | <= 600 KB | <= 400 KB | `ls -l` stripped ELF (initial target; confirm by measurement in Phase 6, change only via ADR) |
| **`/bin/sh` (`dash`, static)** | <= 128 KB | <= 96 KB | `ls -l` stripped ELF |
| **`awk` (`onetrue-awk`, static)** | <= 160 KB | <= 128 KB | `ls -l` stripped ELF |
| **Minimal bootable system image (kernel + servers + initramfs)** | <= 4 MB uncompressed | <= 2.5 MB | Image artifact size |
| **Idle boot-to-shell RAM consumption** | <= 16 MB | <= 8 MB | `procd` untyped free-memory report |
| **Cold boot to `/bin/sh` prompt (QEMU TCG / HVF)** | <= 1.5 s (TCG) / <= 200 ms (HVF) | <= 100 ms (HVF) | Serial timestamp from entry to prompt |
| **VDSO syscalls (`getpid`, `clock_gettime`)** | <= 20 ns (hardware/HVF) | <= 10 ns | Microbenchmark loop (10M iters) |
| **Null IPC syscall round-trip (`procd` fastpath)** | <= 600 cycles (hardware) | <= 400 cycles | Cycle counter (`CNTVCT_EL0`/`PMCCNTR`) |
| **Pipe throughput (64 KB chunks, SPSC ring)** | >= 4 GB/s (hardware/HVF) | >= 8 GB/s | `bench_pipe` |
| **Uncontended mutex lock/unlock** | <= 15 ns (0 IPCs) | <= 8 ns | `bench_futex` |
| **`posix_spawn` + `/bin/true` + `waitpid`** | <= 250 us (hardware/HVF) | <= 120 us | `bench_spawn` |
| **POSIX conformance (`libc-test` + OPTS core + `pjdfstest`)** | >= 99% pass | 100% pass (excl. optional XSI) | Automated QEMU test harness |

### 1.5 Explicit Non-Goals (Scope Discipline)
To keep Fastix tiny, clean, and fast, the following are explicitly OUT of scope for the core OS:
- No Linux binary ABI emulation (`ELF` Linux syscalls) — Fastix targets source-level ISO C / POSIX compatibility with a clean native ABI.
- No monolithic desktop environment, Wayland/X11, or audio stack in the base OS (can be optional third-party ports later).
- No Systemd-style bloated init ecosystem — PID 1 is a tiny deterministic C supervisor.
- No non-C languages in the base OS servers or base userland (keeps toolchain minimal, builds fast, footprint tiny).
- No custom hacks inside the seL4 kernel source tree.

---

## 2. Agent & Engineering Rules of Engagement

Every agent or contributor MUST obey these rules on every task:

1. **Plan Before Editing:** Before modifying any subsystem, restate the exact sub-goal, identify all affected interfaces/headers, and list edge cases.
2. **Single Source of Truth:** Never duplicate a syscall number, slot index, page size, max path length, or struct layout across two files. Put shared definitions in `include/fastix/` and include them everywhere.
3. **Strict Compilation Gate:**
   - Mandatory flags for all Fastix-authored C code (`libos`, servers, drivers, userland):
     `-std=c17 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wformat=2 -Wundef -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -fno-common -fno-strict-aliasing`
   - Exception: code that includes seL4 headers (`libos`, servers, drivers, `libfastix_sys`) uses `-std=gnu17` instead of `-std=c17`, because `libsel4` headers use the GNU `asm` keyword. All other flags stay the same. Userland stays strict `-std=c17`.
   - Additional hardening in debug/test builds: `-fstack-protector-strong -DFASTIX_DEBUG=1` (plus `-fsanitize=address,undefined` for host-compiled unit tests).
   - Release builds: `-Os` (size-critical userland/cold server paths) or `-O2` (hot IPC/VFS/network paths), `-flto=thin`, `-ffunction-sections -fdata-sections`, linker `--gc-sections`.
   - Third-party code (`seL4`, `musl`) compiles with upstream flags without modifying upstream warnings unless upstreamed cleanly.
   - Third-party headers (`libsel4`, generated seL4 headers, musl internals) are ALWAYS included via `-isystem`, never `-I`, so `-Wpedantic -Werror` applies only to Fastix-authored code and an upstream header bump can never break the Fastix build.
4. **Memory & Safety Discipline:**
   - Every function returning allocated resources or errors uses explicit `int` (`0` on success, negative `-ERRNO` on failure) or explicit `fastix_err_t`. Never silently ignore return values (`__attribute__((warn_unused_result))`).
   - All user pointers passed to servers MUST be validated against process VMA bounds and permissions before access; never trust user-supplied lengths, indices, or null-termination.
   - No unbounded recursion in any server (`procd`, `vfsd`, `netd`, `devd`, `ttyd`). Fixed small server stacks (16–32 KB) with guard pages.
   - Zero dynamic allocation inside IRQ handlers or seL4 fault fastpaths (pre-allocated slabs only).
5. **Lock Ordering & Deadlock Prevention:**
   - Every cross-server IPC MUST follow a strict acyclic call hierarchy (defined in Section 14.5). A lower-tier server MUST NEVER make a synchronous `seL4_Call` back to a higher-tier server (use asynchronous `seL4_Notification` or client-driven retries instead).
   - Every internal lock inside a server MUST declare a numeric rank and acquire locks strictly in ascending rank order (verified at runtime in debug builds).
6. **Self-Review Before Completing Any Task:**
   - Re-read all changed files looking for: duplicated logic, hidden cross-file coupling, magic numbers, dead code, integer overflows (`size * count`), signed/unsigned bugs, off-by-one bounds, uninitialized struct padding (info leaks across address spaces!), and zero/empty/max edge cases.
7. **Honest Reporting Format:**
   Every engineering report or PR summary MUST end with:
   - **VERIFIED:** Exact commands run, tests executed, and observed numeric outputs.
   - **NOT VERIFIED:** Anything assumed, skipped, or unable to test in the current environment.
   - **KNOWN ISSUES:** Real bugs, limitations, or edge cases observed and not yet fixed.
   Never claim "verified", "fixed", or "works" without observed evidence.
8. **Explain Simply:** The project owner is new to OS development. Focus on building the project, but at the end of each task explain what was done and why in short, plain, simple words (define any technical term in one sentence the first time it appears). No lessons, quizzes, or exercises unless the owner asks.
9. **Simple Code First:** Follow Section 2.1 on every change.
10. **Keep `PROGRESS.md` Up to Date:** Read `PROGRESS.md` at the start of every task and update it at the end of every task, in the same change as the code. It is the single place where the owner (and the next agent) sees where the project stands. Format:
    - **Current Phase:** phase number + name, and one sentence on its status.
    - **Phase Checklist:** every exit criterion of the current phase (Section 12) as a checkbox: `[x]` only with evidence (test command + observed result), `[ ]` otherwise.
    - **Done Log:** newest first, one line per task: date, what was done in plain words, how it was verified.
    - **Next Steps:** the next 1–3 tasks, in order.
    - **Known Issues & Open Questions:** real bugs and decisions waiting for the owner.
    - **Key Numbers:** latest measured sizes and benchmark results vs Section 1.4 budgets (state QEMU TCG / HVF / hardware).
    Keep it short (target under ~150 lines): when a phase completes, collapse its Done Log into a 3–5 line summary.
11. **Match the House Style:** Follow `FORMAT.md` (workflow, code style, tests, commits, and report format) so every agent's work looks the same.

### 2.1 Code Simplicity, Speed & Maintainability Rules
Fastix code MUST be simple enough that a new reader understands any function in about two minutes, AND fast. These rules make both possible:

1. **Simplest Correct Version First:** Write the plain, obvious implementation first and make it pass its tests. Optimize only after a `bench/` measurement shows that code is on a hot path, and keep the before/after numbers in the commit message. "Measure, then optimize." Never optimize by guessing.
2. **Design for Speed, Implement Simply:** Speed decisions that are expensive to retrofit are part of the architecture from Day 1: the async server loop with saved reply caps, shared-memory rings, the VDSO, the badge layout, and the FD ownership protocol (Section 6.4). Everything else (warm object pools, which sit behind the object allocator API so callers never change; SIMD `memcpy`, superpages, read-ahead, cache-line tuning, hash table variants) starts as simple C and is upgraded only when a benchmark shows it is needed.
3. **Small Pieces, One Job Each:** Functions do one thing and are short (target <= 50 lines; a longer function needs a reason). Files cover one topic (target <= 800 lines). Each server is split into clear modules (e.g. `procd/{proc,vm,sig,ipc}`).
4. **Plain C, No Cleverness:** No macro tricks, no hidden control flow, no deep pointer-to-function webs, no code generation unless unavoidable. Prefer fixed-size arrays and simple loops over complex data structures when the size is small and bounded. Use `static inline` functions instead of function-like macros.
5. **Fast Paths Stay Small and Have a Simple Twin:** Every fast path (VDSO read, ring read/write, futex fast path, IPC fast path) is a small, clearly named function next to a simple slow path that is always correct. Tests check that both paths give identical results.
6. **No Speculative Code:** No features, options, or abstractions "for later." An abstraction is added only once it has at least two real users. Delete dead code immediately; deleting code is a good change.
7. **Clear Names, Few Comments:** Names say what things are and do (`vma_split`, not `vs2`). Comments explain *why* something is done, not *what* the code says. Each file starts with a 1–3 line comment saying its job.
8. **One Error Style Everywhere:** Functions return `0` or a positive value on success and `-ERRNO` on failure (Section 2, rule 4). Errors are handled with early returns and a single cleanup path per function; no deeply nested `if` chains.
9. **Each Server Has a Short README:** `servers/<name>/README.md` lists what the server does, its IPC operations, its main data structures, and its locks/ranks, in under one page. It is updated in the same change as the code.
10. **Consistent Formatting:** One `.clang-format` file at the repo root, applied to all Fastix-authored code; `clang-tidy` with a small, fixed check list runs in CI. Formatting is never discussed in review.
11. **Review Question:** Every change is checked with: "Is there a simpler way that is just as fast?" If yes, use it.

---

## 3. Target Architectures & Host Development Environment

### 3.1 Target Platform Priority

| Tier | Architecture | Platform / Machine | seL4 Kernel Config | Rationale |
| :--- | :--- | :--- | :--- | :--- |
| **Tier 1 (Primary)** | `aarch64` (ARMv8-A) | QEMU `virt` (Cortex-A53 / Cortex-A72, GICv2/v3, PL011, VirtIO-MMIO) | `AARCH64`, `qemu-arm-virt`, `KernelIsMCS=ON` | Formally verified 64-bit seL4 target, clean 48-bit VA, fast native/TCG execution on Apple Silicon macOS host, simple MMIO device tree. |
| **Tier 2** | `x86_64` | QEMU `q35` / `pc99` (APIC, HPET, COM1, VirtIO-PCI) | `X86_64`, `pc99`, `KernelIsMCS=ON` | Broad desktop/server/VM portability; validates architecture abstraction cleanliness. |
| **Tier 3 (Future)** | `aarch64` hardware & `riscv64` | Raspberry Pi 4/5, QEMU `riscv64-virt` | Upstream seL4 board configs | Real bare-metal benchmarking and RISC-V portability. |

### 3.2 Verified Host Environment & Required Toolchain Setup
Current host workspace (`/Users/thamer/my stuff/fastix`) runs on **Apple Silicon macOS (`arm64`, macOS 15.7.7)**.

- **Already present on host (verified):**
  - `cmake`, `ninja`, `python3`, `git`, `dtc` (Device Tree Compiler), `cpio`
  - `qemu-system-aarch64`, `qemu-system-x86_64`
  - Homebrew LLVM (`clang`, `llvm-ar`, `llvm-objcopy` under `/opt/homebrew/opt/llvm/bin/`)
- **Missing on host and MUST be installed in Phase 0:**
  - `lld` (`ld.lld` — required for cross-linking ELF64 binaries via Clang on macOS, since Apple's Mach-O `ld` cannot link ELF)
  - Optional GNU cross-toolchains: `aarch64-elf-gcc` / `aarch64-elf-binutils` (and `x86_64-elf-gcc` for Tier 2) — seL4's CMake build historically works most smoothly with either `aarch64-none-elf-gcc` or Clang + `lld` + `llvm-objcopy`
  - Python 3 virtual environment (`.venv/`) with seL4 build dependencies (`jinja2`, `pyyaml`, `ply`, `pyelftools`, `six`, `sh`, `protobuf`, `lxml`, `sel4-deps`)
  - Optional: `mtools` / `e2fsprogs` (`mkfs.ext2`, `mkfs.ext4`, `debugfs`) for creating and inspecting disk images on host without root privileges
  - Optional fallback: Lima / OrbStack / Docker Linux builder if any upstream seL4 script assumes GNU `coreutils`/`sed` behavior (prefer native macOS cross-build first, keeping all build scripts POSIX-portable).

- **Phase 0 result (2026-11-19):** installed Homebrew `lld` (23.1.6) and GNU `cpio` (2.15, keg-only, required by seL4's `MakeCPIO`); Python deps pinned in `tools/requirements.txt` (adds `lxml` and `libarchive-c`). No GNU cross-GCC or Linux container was needed. `tools/build.sh` is the single build command. Details: `docs/adr/0001-sel4-config.md`.

### 3.3 Architecture Abstraction Boundary
All architecture-dependent code MUST live strictly inside `arch/<arch>/` (`arch/aarch64/`, `arch/x86_64/`) and expose one unified internal interface:
- Hardware page constants (`PAGE_SHIFT`, `PAGE_SIZE`, `PAGE_LEVELS`, `VA_BITS`, canonical masks)
- Register context layout (`fastix_regs_t`, `fastix_fpu_t`, instruction pointer, stack pointer, return value register, arg registers)
- Hardware counter & timer access (`arch_read_counter()`, `arch_counter_freq()`, memory barriers, `arch_pause()`)
- TLS base pointer read/write (`tpidr_el0` on AArch64; `FS_BASE` MSR / `seL4_TCB_SetTLSBase` on x86_64)
- Signal frame push/pop, stack alignment (16-byte on both; 128-byte red zone skip on x86_64), and VDSO `sigreturn` trampoline machine code
- seL4 architecture-specific page-table mapping wrappers (PGD/PUD/PD/PT levels vs PML4/PDPT/PD/PT)

---

## 4. Repository Layout & Dependency DAG

### 4.1 Directory Structure

```text
fastix/
├── AGENTS.md                       # This master specification & roadmap
├── PROGRESS.md                     # Live project status: current phase, checklist, done log, next steps (rule 10)
├── CMakeLists.txt                  # Top-level superbuild orchestrator
├── config/
│   ├── aarch64-qemu-virt.cmake     # Target board & seL4 configuration presets
│   ├── x86_64-qemu-q35.cmake
│   └── fastix_defaults.h.in        # Build-time feature toggles & limits
├── third_party/
│   ├── seL4/                       # Unmodified upstream seL4 kernel (git submodule/pinned commit)
│   ├── seL4_tools/                 # elfloader-tool & cmake-tool (pinned commit)
│   ├── musl/                       # Upstream musl libc + minimal Fastix arch/port overlay
│   ├── toybox/                     # Upstream toybox (0BSD), pinned commit
│   ├── dash/                       # Upstream dash (BSD), pinned release
│   ├── onetrue-awk/                # Upstream "The One True Awk" (BWK awk), pinned commit
│   └── tests/                      # libc-test, OPTS subset, pjdfstest (pinned commits)
├── include/
│   └── fastix/                     # SINGLE SOURCE OF TRUTH for all shared ABIs & constants
│       ├── config.h                # Global limits, pool sizes, page/buffer sizes, feature flags
│       ├── abi.h                   # Syscall numbers, IPC opcodes, message register layouts
│       ├── cspace.h                # Fixed root CNode slot indices & badge encoding rules
│       ├── vspace.h                # Canonical virtual address layout constants
│       ├── errno.h                 # Canonical POSIX errno values (aligned with musl)
│       ├── vdso.h                  # struct fastix_vdso layout & seqlock protocol
│       ├── ring.h                  # Lock-free shared-memory SPSC/MPSC ring wire format
│       ├── signal_abi.h            # Signal frame, ucontext, trampoline ABI
│       ├── vfs_proto.h             # procd <-> vfsd & client <-> vfsd IPC protocol
│       ├── dev_proto.h             # devd <-> drivers <-> vfsd/netd IPC protocol
│       ├── net_proto.h             # client <-> netd socket & epoll IPC protocol
│       ├── tty_proto.h             # client <-> ttyd termios/PTY IPC protocol
│       └── lock_rank.h             # Strict global lock ordering ranks
├── arch/
│   ├── aarch64/                    # AArch64 trap/regs/TLS/paging/VDSO/atomics/sigtramp
│   └── x86_64/                     # x86_64 trap/regs/TLS/paging/VDSO/atomics/sigtramp
├── libs/
│   ├── libos/                      # Tiny freestanding C17 runtime for OS servers (NO POSIX dep)
│   │   ├── alloc/                  # Slab allocator, buddy untyped allocator, arena allocator
│   │   ├── cap/                    # CSpace slot allocator & object pool manager
│   │   ├── ipc/                    # seL4 MCS Call/ReplyRecv wrappers, badge helpers, cap transfer
│   │   ├── ring/                   # Lock-free shared-memory SPSC/MPSC ring implementation
│   │   ├── ds/                     # Intrusive lists, hash tables, radix tree, interval/VMA tree
│   │   ├── sync/                   # Spinlocks, rank-checked mutexes, notification waitqueues
│   │   ├── fmt/                    # Minimal printf/snprintf/logging & ring flight-recorder
│   │   └── mem/                    # Fast memcpy/memset/memmove/memcmp/strlen (portable + SIMD)
│   ├── libdriver/                  # Shared driver SDK: MMIO, DMA pools, IRQ handlers, DTB/PCI
│   └── libfastix_sys/              # Userland low-level syscall/IPC/VDSO glue (linked into musl)
├── servers/
│   ├── procd/                      # Root task + Process, Thread, VSpace, COW, Signal, Futex, Pipe
│   ├── vfsd/                       # VFS core, dentry/inode cache, unified page cache, filesystems
│   │   ├── core/                   # Mounts, path walk, FD/file table, flock/fcntl, page cache
│   │   └── fs/                     # cpiofs, tmpfs, devfs, procfs, ext2, ext4, fat32, virtiofs
│   ├── devd/                       # Hardware discovery (DTB/ACPI), capability broker, driver supervisor
│   ├── ttyd/                       # Console, termios line discipline, job control, PTY master/slave
│   └── netd/                       # AF_UNIX (SCM_RIGHTS), TCP/IPv4/IPv6, UDP, ICMP, ARP/NDP, epoll
├── drivers/
│   ├── uart_pl011/                 # AArch64 QEMU serial console
│   ├── uart_16550/                 # x86_64 serial console
│   ├── timer/                      # Hardware timer + wall-clock RTC (pl031 / CMOS)
│   ├── virtio_blk/                 # VirtIO block device driver (DMA rings)
│   ├── virtio_net/                 # VirtIO network device driver (zero-copy RX/TX rings)
│   ├── virtio_rng/                 # Hardware entropy source for /dev/random & AT_RANDOM
│   └── virtio_fs/                  # Optional host-shared filesystem for rapid dev
├── libc/
│   ├── musl_port/                  # Fastix syscall backend, crt0, VDSO hooks, thread/signal glue
│   └── fastix-cc/                  # Cross-compiler & sysroot wrapper script/driver
├── userland/
│   ├── init/                       # PID 1 minimal init, rc runner, orphan reaper, shutdown (Fastix-authored)
│   ├── toybox/                     # Fastix toybox build: fastix.config, command allowlist, patches/
│   ├── dash/                       # Fastix dash build recipe + patches/ (/bin/sh)
│   ├── awk/                        # onetrue-awk build recipe + patches/
│   ├── extra/                      # Small Fastix-authored tools ONLY for gaps no permissive upstream fills (e.g. reboot/poweroff glue)
│   └── ports/                      # Build recipes for 3rd-party C apps (lua, sqlite, tcc, make, dropbear, vim, git, ...)
├── rootfs/
│   ├── etc/                        # Minimal /etc/{rc,passwd,group,hosts,resolv.conf,profile,fstab}
│   └── mkrootfs.py                 # Deterministic CPIO initramfs & ext2/ext4 image builder
├── tests/
│   ├── host/                       # Host-compiled unit + randomized stress tests (ASan/UBSan)
│   ├── kernel_ipc/                 # Bare seL4 + procd IPC, cap leak, COW, fault tests
│   ├── posix/                      # Native POSIX behavior, signal, PTY, VFS, socket, thread tests
│   ├── chaos/                      # OOM, fork-bomb, driver crash recovery, syscall fuzzer
│   └── run_qemu_tests.py           # Headless QEMU runner with timeout, log parser & exit code
├── bench/
│   ├── micro/                      # Null syscall, VDSO, pipe, fork, spawn, fault, futex, stat
│   └── macro/                      # Build speed, SQLite, shell pipeline throughput
├── tools/                          # Toolchain installer, symbolizer, QEMU launcher, image inspector
└── docs/                           # Architecture decision records (ADRs) & hardware bring-up notes
```

### 4.2 Strict Dependency DAG (Zero Circular Dependencies)
```text
include/fastix/*  (pure headers, zero dependencies)
        │
        ▼
arch/<arch>/*  ──►  seL4 headers (libsel4)
        │
        ▼
libs/libos  (freestanding C17, depends ONLY on include/fastix, arch, libsel4)
   │           │
   ▼           ▼
libs/libdriver  servers/{procd, vfsd, devd, ttyd, netd}
   │
   ▼
drivers/*  (isolated processes, depend ONLY on libos + libdriver)

--- Clean ABI Boundary (seL4 IPC + Shared Memory + VDSO) ---

libs/libfastix_sys  (depends ONLY on include/fastix, arch, libsel4)
        │
        ▼
libc (musl port)  ──►  sysroot/usr/{include,lib}
        │
        ▼
userland/{init, toybox, dash, awk, extra, ports}  (pure standard C + POSIX ONLY; ZERO seL4 includes!)
```

**Hard Rule:** Nothing under `userland/` may ever `#include <sel4/...>`. Userland programs speak strictly ISO C and POSIX through `libc`. This guarantees that standard C programs work identically to native Fastix userland.

### 4.3 Source Repository
- **Home:** https://github.com/gg6789t/fastix (public). Default branch: `main`.
- **Clone** with the pinned third-party sources (seL4 17.0.0, `seL4_tools`, `util_libs` as git submodules):
  `git clone --recursive https://github.com/gg6789t/fastix.git`
  (`tools/build.sh` also runs `git submodule update --init` if they are missing).
- **Build and test:** `tools/build.sh`, `tools/test_host.sh`, `python3 tests/run_qemu_tests.py`. Host setup is in Section 3.2 and `docs/adr/0001-sel4-config.md`.
- **Where things stand:** `PROGRESS.md` at the repo root.
- **Commits and pushes:** only when the owner asks (`FORMAT.md` Section 6), normally one commit per finished phase or task, pushed to `origin main`. Never commit `build/`, `build-host/`, `.venv/`, or secrets. Never edit `third_party/` (submodules stay pinned).
- **The repo is public:** everything committed (including commit author names and emails) is visible to anyone.

---

## 5. Micro-Level Foundations (The "Tiny Things")

Every microkernel OS lives or dies by its lowest-level mechanisms. Every single low-level detail is specified here before a line of code is written.

### 5.1 seL4 Kernel Configuration Details
- **Kernel Variant:** seL4 with **MCS** (`KernelIsMCS=ON`).
  - Why MCS? MCS separates threads (`TCB`) from CPU budgets (`SchedulingContext`), enables passive servers that run on the caller's time slice (eliminating priority inversion and scheduler ping-pong on IPC!), provides `seL4_Reply` objects for clean non-blocking server reply management, and supports timeouts on blocking notifications (`seL4_Wait` / `seL4_NBSendRecv`).
- **Verification Scope (Be Honest):** seL4's proofs cover specific configurations only. MCS and SMP (`KernelMaxNumNodes > 1`) may not be inside the proven configuration set for the chosen architecture at the pinned commit. Phase 0 MUST record in `docs/adr/0001-sel4-config.md` exactly which proofs (functional correctness, integrity, confidentiality, binary verification) cover the pinned commit + config, and documentation MUST say "built on seL4" rather than "formally verified" for any configuration outside that set. If a proven non-MCS configuration is required later, the only MCS-specific code (passive servers, `seL4_Reply` objects, SC budgets) is isolated behind `libs/libos/ipc/` so it can be swapped.
- **Fastpath:** Ensure `KernelFastpath=ON`. All hot-path IPCs (`procd`, `vfsd`, `netd`, `ttyd`) MUST fit within seL4 fastpath constraints:
  - `seL4_Call` from client, `seL4_ReplyRecv` from server.
  - Message length <= 4 registers (`MR0..MR3`) for fastpath calls where possible; no capability transfer on hot fastpath calls (data rides in shared memory or `MR0..MR3`).
  - Exact IPC buffer alignment (1024-byte aligned on 64-bit) and valid mapped VSpace.
- **Root Task (`procd` bootstrap):**
  - seL4 boots a single initial thread with `seL4_BootInfo` containing all Untyped memory descriptors, initial CNode slots, and available ASID pools.
  - `procd` IS the root task (eliminating an extra hop between a separate rootserver and process manager). On startup, `procd` initializes its untyped allocator, sets up its own fault/syscall endpoints, spawns `devd`, `vfsd`, `ttyd`, `netd` as isolated child protection domains with tightly scoped capabilities, and then spawns PID 1 (`/sbin/init`).
- **SMP Readiness (Built-in from Day 1, activated in Phase 9):**
  - `KernelMaxNumNodes=1` in Phases 0–8 for deterministic debugging; `KernelMaxNumNodes=4` (up to 16) in Phase 9.
  - All shared-memory rings, VDSO seqlocks, refcounts, and futex words MUST use C11 `<stdatomic.h>` memory orders (`acquire`/`release`/`acq_rel`/`seq_cst`) from Day 1 — never plain volatile reads/writes.

### 5.2 Physical Memory & Untyped Allocator (`procd`)
seL4 has no kernel heap; every kernel object (TCB, CNode, Endpoint, Notification, Reply, SC, PageTable, 4 KB Frame, 2 MB Superpage) is created by retyping Untyped memory capabilities.
- **Untyped Inventory:** Parse `seL4_BootInfo->untypedList` at boot. Separate **device untypeds** (MMIO regions, handed exclusively to `devd`) from **RAM untypeds** (managed by `procd`).
- **Buddy Untyped Allocator:**
  - Split large RAM untypeds down to power-of-two blocks on demand (`2 MB` blocks for superpages and slab chunks; `4 KB` frames for normal pages; small blocks for TCBs/EPs/NTFNs/Replies/SCs).
  - Track every allocated physical frame in a flat **Physical Frame Metadata Array** (`struct phys_frame`), indexed by physical page number (PPN):
    - `uint32_t refcount` (for COW & page-cache sharing)
    - `uint16_t flags` (`FRAME_ANON`, `FRAME_FILE_CACHE`, `FRAME_DMA`, `FRAME_PINNED`, `FRAME_ZEROED`, `FRAME_COW`)
    - `seL4_CPtr cap` (root CNode capability to the 4 KB `seL4_FrameObject`)
    - `uint32_t next_free` (intrusive freelist index)
  - Overhead: 16 bytes per 4 KB frame = 0.39% of RAM (only 1 MB for 256 MB RAM).
- **Warm Object Recycling Pools (Speed Secret):**
  - Never call `seL4_Untyped_Retype` + `seL4_CNode_Revoke` on every short-lived `fork`/`exec`!
  - `procd` maintains O(1) warm freelists of already-retyped, scrubbed objects:
    - Free 4 KB zeroed frames (background/lazy zeroing pool)
    - Free TCBs + Reply objects + Notification objects + SchedulingContexts
    - Free Page Table objects (L1/L2/L3)
    - Free per-process CNodes
  - Only revoke/coalesce back to Untyped when a pool exceeds its high-water mark or another object class hits low-water mark (memory pressure reclamation).
- **Strict Zero-Leak Guarantee:**
  - Every `struct proc` owns an intrusive list of every kernel object capability it holds (VSpace, PageTables, CNode, TCBs, SCs, Notifications, private frames).
  - On process destruction, walking that list deterministically unmaps frames, decrements refcounts (returning 0-ref frames to the free pool), resets TCBs/CNodes, and returns every object to its warm pool.
  - Debug builds verify `total_allocated_objects` invariants on every `waitpid()` reap.

### 5.3 Canonical Virtual Address Space Layout (48-bit VA)

Both AArch64 (`0x0000_0000_0000_0000` – `0x0000_FFFF_FFFF_FFFF`) and x86_64 (`0x0000_0000_0000_0000` – `0x0000_7FFF_FFFF_FFFF`) share a clean, uniform user virtual address layout (exact values frozen in `include/fastix/vspace.h`):

| Region | Address Range (Example 47-bit User Canonical) | Permissions | Purpose |
| :--- | :--- | :--- | :--- |
| **Null Guard** | `0x0000_0000_0000` – `0x0000_0010_0000` (1 MB) | `---` (unmapped) | Catches NULL and small-offset dereferences deterministically (`SIGSEGV`). |
| **Executable ELF (Non-PIE)** | `0x0000_0040_0000` – `0x0000_4000_0000` | `R-X` / `R--` / `RW-` | Static / non-PIE `.text`, `.rodata`, `.data`, `.bss`. |
| **Program Break (`brk`/heap)** | Directly after `.bss` (page-aligned), growing up | `RW-` | Classic `brk()`/`sbrk()` heap (guarded from colliding with `mmap`). |
| **PIE / Dynamic Linker Base** | `0x0001_0000_0000`+ (with optional ASLR offset) | `R-X` / `R--` / `RW-` | PIE main executables and `ld-musl-<arch>-fastix.so.1`. |
| **Dynamic `mmap` Region** | `0x0010_0000_0000` – `0x006F_0000_0000` (ends 4 GB below the stack top so the main stack can grow) | Per-VMA | `mmap` anonymous, file-backed, shared libs, thread stacks (with 4 KB `PROT_NONE` guard pages), `shm`. |
| **Main Thread Stack** | Top at `0x0070_0000_0000`, grows down (default 8 MB `RLIMIT_STACK`) | `RW-` + `---` guard | Initial `argc`/`argv`/`envp`/`auxv`, demand-grown on fault down to limit; guard page below triggers `SIGSEGV`. |
| **Global VDSO Page (RO)** | `0x007F_F000_0000` (1–2 pages) | `R-X` (code) + `R--` (data) | Shared read-only clock timebase, VDSO helper code, and `sigreturn` trampoline. |
| **Per-Process Info Page (RO)** | `0x007F_F001_0000` (1 page) | `R--` | Fast read-only `pid`, `ppid`, `uid`, `euid`, `gid`, `egid`, `pgid`, `sid`, `umask`. |
| **Per-Thread IPC Buffers** | `0x007F_F100_0000` + `tid_slot * 4096` | `RW-` | seL4 `seL4_IPCBuffer` for each thread (1 page per thread, up to `FASTIX_MAX_THREADS_PER_PROC = 1024`). |
| **Per-Thread Shared I/O Window** | `0x007F_F200_0000` + `tid_slot * 65536` | `RW-` | Lazily mapped per-thread 64 KB shared I/O buffer for `procd`/`vfsd`/`netd`/`ttyd` (zero cross-thread clobbering); dynamic pipe/socket rings mapped in `mmap` region. |

**Tiny Details Checked:**
- **W^X Enforcement:** No user mapping is ever mapped both Writable and Executable by default (unless an explicit JIT requests `mprotect(PROT_READ|PROT_EXEC)` after writing, or explicit `PROT_WRITE|PROT_EXEC` if permitted by policy).
- **Zero-Fill Security:** Every newly mapped anonymous physical frame MUST be zeroed before first exposure to any user process (prevents cross-process information leakage).
- **2 MB Superpages:** Large anonymous/file mappings (>= 2 MB, aligned) automatically promote to 2 MB seL4 large pages to cut TLB misses on big allocations and large files.

### 5.4 CSpace (Capability Space) Layout & Unforgeable Badges
Every POSIX user process gets a 2-level CSpace whose layout NEVER changes after creation, so CPtrs cached in TLS and in `libfastix_sys` are stable for the life of the process (no "CNode promotion", which would invalidate every cached CPtr across all threads). The root CNode is tiny (`2^6 = 64` slots = 2 KB). Slots `0..47` hold capabilities directly; slots `62` and `63` hold CNode capabilities to lazily allocated level-2 CNodes that cost nothing until first used. The root CNode guard is sized so both a level-1 slot and a level-2 slot resolve with a full 64-bit CPtr depth; CPtr encode/decode helpers live ONLY in `cspace.h`. All threads of a process share this one CSpace. Userland NEVER manually manages raw seL4 capabilities — every fixed slot is frozen in `include/fastix/cspace.h`:

| Slot Index | Constant Name | Capability Type | Rights | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| `0` | `CAP_NULL` | Null | None | Invalid capability sentinel. |
| `1` | `CAP_PROCD_EP_MAIN` | Badged Endpoint | `Write \| GrantReply` | Main thread syscall IPC to `procd`. Badge uses Section 14.2 64-bit layout `(type \| pid \| tid_slot \| gen)`. |
| `2` | `CAP_VFSD_EP` | Badged Endpoint | `Write \| GrantReply` | Direct fastpath VFS IPC to `vfsd`. Badge encodes `vfs_client_id`. |
| `3` | `CAP_NETD_EP` | Badged Endpoint | `Write \| GrantReply` | Direct fastpath Socket/Poll IPC to `netd`. Badge encodes `net_client_id`. |
| `4` | `CAP_TTYD_EP` | Badged Endpoint | `Write \| GrantReply` | Direct fastpath TTY/PTY IPC to `ttyd`. Badge encodes `tty_client_id`. |
| `5` | `CAP_MAIN_NTFN` | Notification | `Read` | Main thread wakeup notification (for futex/signal/ring wakeup). |
| `6..15` | `CAP_RESERVED_SERVERS` | Endpoints | Reserved | Future direct fastpath services (e.g., audio/gpu/ipc-bus). |
| `16..47` | `CAP_DYN_RINGS_L1` | Notifications | `Read \| Write` | First 32 per-FD notification caps for SPSC pipes / eventfd / Unix sockets (covers almost every process with zero extra CNode). |
| `48..61` | `CAP_RESERVED_L1` | — | — | Reserved for future fixed capabilities. |
| `62` | `CAP_DYN_RINGS_CNODE` | CNode (`2^10` slots) | — | Level-2 CNode, allocated on the 33rd ring cap; holds up to 1024 more per-FD notification caps. |
| `63` | `CAP_THREAD_CNODE` | CNode (`2^11` slots) | — | Level-2 CNode, allocated on first `pthread_create`; slot `2*tid_slot` = thread's badged `procd` EP, `2*tid_slot+1` = thread's notification (CPtrs cached in each thread's TLS struct for O(1) lookup; covers `FASTIX_MAX_THREADS_PER_PROC = 1024`). The main thread keeps using slots `1` and `5`. |

**Security Property:** Because seL4 enforces capability badges in hardware/kernel, a process CANNOT spoof its `pid`, `tid`, or `client_id` to any server! When `vfsd` receives an IPC, seL4 delivers the unforgeable `badge` directly in a register, giving O(1) array lookup `clients[badge]` with zero authentication overhead.

### 5.5 IPC Wire Protocol & Fastpath Message Layout
Every server IPC follows a single uniform ABI (`include/fastix/abi.h`):
- **Request (`seL4_Call`):**
  - `seL4_MessageInfo`: `label = op_code` (12-bit operation ID), `length = N` (0..6 words for fastpath; up to 16 words max), `extraCaps = 0` (except rare FD/cap passing ops).
  - `MR0..MR5`: Direct 64-bit integer arguments (pointers, sizes, flags, offsets, FD numbers).
  - **Short-path inline data:** Short paths/strings (<= 64 bytes) or small reads/writes (<= 48 bytes) MAY travel inline in `MR`s to avoid touching the shared buffer cache line; anything larger uses the pre-mapped **Shared I/O Data Window** (offset + length passed in `MR`s).
- **Reply (`seL4_ReplyRecv`):**
  - `MR0`: `int64_t ret` — `>= 0` on success, `-ERRNO` on failure (exact POSIX errno from `include/fastix/errno.h`).
  - `MR1`: Secondary return value if needed (e.g., `pipe` fd pair, `fork` extra info, `lseek` 64-bit offset, `recvfrom` address len).
  - `MR2`: Pending signal bitmask hint (piggybacked on every `procd` reply so userland knows immediately if an unblocked signal arrived!).
- **Blocking & Interruption (`EINTR` / `SA_RESTART`) — Critical POSIX Detail:**
  - Servers MUST NEVER block their main event loop waiting for slow I/O (disk, network, pipe, TTY, timer, `waitpid`).
  - When a client syscall cannot complete immediately:
    1. The server saves the client's **Reply Capability** (`seL4_Reply` object slot from a server pool) into a waiter struct (`struct waiter`) attached to the target object (pipe, socket, disk request, timer, child process).
    2. The server immediately returns to its main `seL4_Recv` loop to serve other clients!
    3. When the event completes (IRQ, peer write, timer tick, or `SIG*` interrupt notification from `procd`), the server pops the waiter and calls `seL4_Send` on the saved Reply capability (returning data or `-EINTR`), freeing the reply slot.
  - This single-threaded event-driven (and later per-core sharded) asynchronous server design achieves **zero server-thread-per-client overhead**, deterministic lock-free state inside a single-core server, and full POSIX `EINTR`/`SA_RESTART` correctness!

### 5.6 Shared-Memory Lock-Free Ring Buffers (`include/fastix/ring.h`)
Used for pipes (`pipe`/`pipe2`), Unix stream sockets, `virtio` driver data planes, and optional batched `io_ring` syscalls:
- **Exact Memory Layout (Cache-Line Isolated to Eliminate False Sharing):**
  - Cache line 0 (`offset 0..63`): Immutable metadata (`magic`, `capacity_mask`, `flags`, `elem_size`).
  - Cache line 1 (`offset 64..127`): Producer-owned `atomic_uint64_t head`, `atomic_uint32_t producer_waiting`, `atomic_uint32_t closed_write`.
  - Cache line 2 (`offset 128..191`): Consumer-owned `atomic_uint64_t tail`, `atomic_uint32_t consumer_waiting`, `atomic_uint32_t closed_read`.
  - Offset `256..`: Power-of-two ring data buffer (default 64 KB = `PIPE_BUF_CAPACITY`, atomic POSIX `PIPE_BUF = 4096`).
- **Notification Suppression Protocol (Zero-Syscall Streaming):**
  - Producer writes bytes, updates `head` with `memory_order_release`, then checks `atomic_load_explicit(&consumer_waiting, memory_order_seq_cst)`. ONLY if `consumer_waiting == 1` does producer call `seL4_Signal(peer_ntfn)`.
  - Consumer reads bytes, updates `tail` with `memory_order_release`, checks `producer_waiting`; ONLY if `1` does it signal producer.
  - When ring is empty, consumer sets `consumer_waiting = 1` (`seq_cst`), re-checks `head != tail` (closes the lost-wakeup race!), and only then waits on its `seL4_Notification` (or falls back to `procd` for `EINTR`-interruptible wait).
  - **Result:** Sustained pipe throughput runs at RAM memcpy bandwidth with near-zero syscalls!

### 5.7 VDSO & Tear-Free Fast Timekeeping (`include/fastix/vdso.h`)
- Mapped read-only at `FASTIX_VDSO_DATA_ADDR` in every process.
- Updated by the timer driver / `procd` via a **seqlock**:
  - `atomic_uint64_t seq` (even = stable, odd = writer updating)
  - `uint64_t mono_base_ns`, `uint64_t realtime_offset_ns`, `uint64_t boottime_offset_ns`
  - `uint64_t counter_base`, `uint64_t mult`, `uint32_t shift` (fixed-point conversion avoiding 128-bit division on hot path)
  - `uint32_t tz_offset_sec`
- Reader loop: read `seq` (acquire) → if odd, spin/yield → read fields + hardware counter (`CNTVCT_EL0` / `RDTSC`) → `atomic_thread_fence(acquire)` → re-read `seq`; retry if changed.
- Per-process read-only page (`FASTIX_PROCINFO_ADDR`): `pid`, `ppid`, `pgid`, `sid`, `uid`, `euid`, `gid`, `egid`, `umask`, updated atomically by `procd` whenever `setuid`/`setpgid`/`setsid`/`fork`/`exec` modifies them.
- **Result:** `getpid()`, `getppid()`, `getuid()`, `geteuid()`, `getgid()`, `getegid()`, `getpgrp()`, `clock_gettime(CLOCK_MONOTONIC|CLOCK_REALTIME|CLOCK_BOOTTIME)`, and `gettimeofday()` execute in **~5–20 nanoseconds with zero kernel entry**.

### 5.8 Hardware Faults, COW & Signal Delivery Micro-Mechanics
Every user thread's seL4 **Fault Endpoint** is configured to `procd`'s badged fault endpoint using the canonical Section 14.2 64-bit badge (`BADGE_TYPE=0x2 | pid | tid_slot | gen`).
1. **Page Fault (`seL4_Fault_VMFault`):**
   - `procd` reads fault address `vaddr` and FSR/access type (read/write/exec).
   - Looks up `vaddr` in the process's VMA interval tree:
     - **Not in any VMA:** Check if just below main stack VMA and within `RLIMIT_STACK` → if so, auto-grow stack (allocate zeroed frame, map `RW-`, resume thread). Otherwise deliver `SIGSEGV` (`SEGV_MAPERR`).
     - **Permission violation:**
       - If write fault on a `VMA_COW` page: if `phys_frame.refcount == 1`, simply upgrade PTE to writable and clear `VMA_COW` (zero copy!); if `refcount > 1`, allocate fresh frame, copy 4 KB, decrement old refcount, remap writable, flush TLB/cache, resume thread.
       - Otherwise deliver `SIGSEGV` (`SEGV_ACCERR`).
     - **Demand-zero anonymous page:** Allocate zeroed frame from warm pool, map, resume.
     - **File-backed `mmap` page:** Request frame capability from `vfsd` page cache for `(inode, file_offset)`. If beyond file EOF → deliver `SIGBUS` (`BUS_ADRERR`). Map read-only (if `MAP_PRIVATE` or `PROT_READ`) or writable (if `MAP_SHARED` + `PROT_WRITE`, marking page dirty in `vfsd`), resume.
2. **CPU Exceptions (`seL4_Fault_UserException`):**
   - Illegal instruction → `SIGILL`; Divide by zero / FP exception → `SIGFPE`; Alignment / bus fault → `SIGBUS`; Breakpoint / debug trap → `SIGTRAP`.
3. **Signal Delivery (`procd` → user thread):**
   - When an unblocked pending signal exists (triggered via `kill`, fault, timer, TTY, or checked on syscall return):
     - If disposition is `SIG_IGN`: discard.
     - If disposition is `SIG_DFL`: perform POSIX default action (Terminate, Core dump/Terminate, Stop, Continue, or Ignore).
     - If user handler registered:
       1. Read thread's register context (`seL4_TCB_ReadRegisters`, including FPU/SIMD state).
       2. Select target stack: `sigaltstack` if `SA_ONSTACK` is set and not already on altstack; otherwise thread's current `SP` (minus 128-byte red zone on x86_64!), aligned down to 16 bytes.
       3. Validate target stack address is writable in user VSpace (if overflowed → force `SIGSEGV` with `SIG_DFL`, preventing infinite fault loops!).
       4. Write `struct fastix_sigframe` onto user stack: `siginfo_t`, `ucontext_t` (saved GP + FPU regs + old signal mask), and `magic`.
       5. Set return address (`LR` on AArch64 / pushed return IP on x86_64) to the VDSO **`__fastix_sigreturn`** trampoline.
       6. Set `PC = handler`, `arg0 = signo`, `arg1 = &frame->info`, `arg2 = &frame->uc`, `SP = frame`.
       7. Update blocked signal mask (`mask |= sa_mask | (SA_NODEFER ? 0 : sigbit)`), write registers back, resume thread.
   - When user handler returns, execution jumps to VDSO `__fastix_sigreturn`, which invokes `SYS_rt_sigreturn` to `procd`; `procd` restores registers + FPU + old signal mask and resumes the interrupted instruction seamlessly!

### 5.9 Process Startup ABI (`_start`, `auxv`, TLS)
When `procd` finishes `execve()`, the initial main thread stack is constructed strictly per the System V ELF ABI (so musl's `_start` and dynamic linker `ld.so` work cleanly):
- 16-byte aligned `SP` pointing to:
  - `argc` (64-bit)
  - `argv[0] .. argv[argc-1]`, `NULL`
  - `envp[0] .. envp[envc-1]`, `NULL`
  - Auxiliary vector pairs (`{type, val}`): `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ` (`4096`), `AT_BASE` (interpreter load addr), `AT_FLAGS`, `AT_ENTRY`, `AT_UID`, `AT_EUID`, `AT_GID`, `AT_EGID`, `AT_SECURE`, `AT_RANDOM` (pointer to 16 cryptographically random bytes from `virtio-rng`/boot entropy), `AT_SYSINFO_EHDR` (VDSO ELF header), `AT_CLKTCK` (`100`), `AT_EXECFN`, terminated by `AT_NULL`.
  - String pool (argv strings, envp strings, execfn, 16 random bytes).
- **Thread-Local Storage (TLS):**
  - AArch64: ELF TLS Variant I (`tpidr_el0` points to TCB; musl sets via `SYS_set_tls` → `seL4_TCB_SetTLSBase`).
  - x86_64: ELF TLS Variant II (`FS_BASE` points to TCB; musl sets via `SYS_set_tls` → `seL4_TCB_SetTLSBase`).

---

## 6. High-Level OS Architecture (The "Big Things")

### 6.1 System Topology Diagram

```text
+-----------------------------------------------------------------------------------+
|                        USERLAND (Pure C17 + POSIX, Zero seL4)                     |
|   /sbin/init   /bin/sh (dash)   /bin/toybox (core tools)   awk   3rd-party C ports  |
+-----------------------------------------------------------------------------------+
|                  musl libc (static libc.a / dynamic libc.so + ld.so)              |
|        libfastix_sys (VDSO fastpath | Lock-free Shm Rings | seL4 IPC Stubs)       |
+==================[ Unforgeable Badged seL4 IPC + Shared Memory ]==================+
|  +------------------+  +------------------+  +---------------+  +--------------+  |
|  |      procd       |  |       vfsd       |  |     netd      |  |     ttyd     |  |
|  | (Root Task/Exec) |  | VFS & Page Cache |  | TCP/IPv4/IPv6 |  | Termios/PTY  |  |
|  | Procs/Threads/VM |  | tmpfs/devfs/ext4 |  | AF_UNIX/epoll |  | Job Control  |  |
|  | COW/Signals/Pipe |  | procfs/cpiofs    |  | Sockets/Loop  |  | Virt Console |  |
|  +--------+---------+  +--------+---------+  +-------+-------+  +------+-------+  |
|           |                     |                    |                 |          |
|           +---------------------+--------------------+-----------------+          |
|                                 | (Device Cap & Data Rings)                       |
|                       +---------+---------+                                       |
|                       |       devd        |  (DTB/PCI Enum & Driver Supervisor)   |
|                       +--+------+------+--+                                       |
|                          |      |      |    (Isolated Per-Device Domains)         |
|                    +-----+  +---+---+  +------+                                   |
|                    | UART|  |VirtBlk|  |VirtNet|  [Timer, RTC, VirtRNG, VirtFS]   |
+====================+=====+==+=======+==+=======+==================================+
|                seL4 Microkernel (Upstream, MCS Enabled, Unmodified)               |
|         Capabilities | Address Spaces | IPC Fastpath | Scheduling | IRQs          |
+-----------------------------------------------------------------------------------+
|            Hardware / QEMU (AArch64 virt / x86_64 q35 / Bare Metal)               |
+-----------------------------------------------------------------------------------+
```

### 6.2 Boot Sequence (Millisecond by Millisecond)
1. **Firmware / QEMU** loads the single `fastix-image.elf` produced by `elfloader-tool`.
2. **`elfloader`** enables MMU, parses DTB, maps seL4 kernel and `procd` (root task) + embedded `initramfs.cpio`, jumps to seL4.
3. **seL4 Kernel** initializes CPU, GIC/APIC, timer, kernel objects, constructs `seL4_BootInfo`, and enters `procd`'s `_start`.
4. **`procd` Early Init:**
   - Parses `BootInfo`; initializes serial early-debug console (disabled once `uart` driver takes over).
   - Builds physical frame metadata array, buddy untyped allocator, warm object pools, VDSO page, and initial random seed.
   - Maps embedded `initramfs.cpio` archive read-only.
5. **Core Server Spawning (Strictly Ordered):**
   - `procd` spawns **`devd`** (granting Device Untyped + IRQControl capabilities) → `devd` parses DTB/PCI and launches isolated **`timer`**, **`uart`**, **`virtio_rng`**, **`virtio_blk`**, **`virtio_net`** drivers.
   - `timer` driver calibrates hardware counter and attaches to VDSO timebase writer.
   - `procd` spawns **`ttyd`** (connected to `uart` driver ring) for `/dev/console`.
   - `procd` spawns **`vfsd`** (granting initramfs frames + `virtio_blk` channel) → `vfsd` mounts `cpiofs`/`tmpfs` root, `/dev` (`devfs`), `/proc` (`procfs`), and optional persistent `ext2`/`ext4` rootfs.
   - `procd` spawns **`netd`** (connected to `virtio_net` ring) → configures `lo` (`127.0.0.1`, `::1`) and `eth0`.
6. **PID 1 (`/sbin/init`):**
   - `procd` creates PID 1 via its standard ELF loader from `/sbin/init` in `vfsd`, opens FD 0/1/2 on `/dev/console`, and starts PID 1.
   - `/sbin/init` runs `/etc/rc` (mounts `/etc/fstab`, configures hostname/network, starts background daemons if configured) and launches interactive `/bin/sh` (`dash`) (or `getty` + `login`).

### 6.3 Core Server Specifications

#### A. `libos` (Tiny Freestanding Runtime for Servers)
- Zero libc/POSIX assumptions; builds freestanding (`-ffreestanding -nostdlib`).
- Components:
  - Fixed-size slab allocator (`16B, 32B, 64B, 128B, 256B, 512B, 1K, 2K, 4K`) backed by seL4 frames.
  - Intrusive doubly-linked lists, open-addressed Robin Hood hash table, red-black/AVL interval tree (for VMAs and timers), radix tree (for page cache and FD tables).
  - Rank-checked spinlocks/mutexes and waitqueues.
  - Minimal structured logger (`klog_info/warn/err`) + in-memory circular flight-recorder buffer (last 4096 events, dumpable on fault or via `/proc/kmsg` / `dmesg`).
  - Optimized memory primitives (`memcpy`, `memset`, `memmove`, `memcmp`) with AArch64 NEON / x86_64 AVX2/SSE2 aligned fastpaths.

#### B. `procd` (Process, Memory, Thread, Signal, Timer, Pipe & Futex Executive)
- **Process & Thread Model:**
  - PID allocator (bitmap, PID `1..32767`, expandable up to 28-bit badge space; PID 1 protected from `SIGKILL`).
  - Process tree (`parent`, `children`, `siblings`), Process Groups (`pgid`), Sessions (`sid`), controlling TTY reference.
  - POSIX threads (`pthread`): threads share VSpace, CSpace, FD table (via `vfsd` process handle), signal dispositions, and credentials; each thread has its own `tid`, TCB, SC, IPC buffer, TLS base, signal mask, `clear_child_tid`, and robust futex list.
- **Virtual Memory Manager:**
  - Per-process VMA interval tree tracking `[start, end)`, `prot`, `flags` (`MAP_PRIVATE`, `MAP_SHARED`, `MAP_ANONYMOUS`, `MAP_FIXED`, `MAP_STACK`), backing file reference `(vfs_file_id, offset)`.
  - Full `mmap`, `munmap` (including splitting existing VMAs cleanly!), `mprotect` (including VMA splitting/merging), `mremap`, `msync`, `madvise` (`MADV_DONTNEED`, `MADV_WILLNEED`), `mincore`, `brk`.
  - **Fast `fork()`:** Clones VMA tree, marks writable private VMAs as `VMA_COW`, downgrades parent PTEs to read-only, duplicates PTEs into child VSpace (skipping empty page tables), increments frame refcounts, and enqueues a single async `VFS_FORK_DUP` work item to `vfsd` (preserving the strict Section 14.5 acyclic rank order; `vfsd` acknowledges downward to `procd` before `procd` replies to parent/child).
  - **Ultra-Fast `vfork()` & `posix_spawn()`:**
    - `vfork()` suspends parent thread, spawns child sharing parent's VSpace temporarily until `execve()` or `_exit()`.
    - `posix_spawn()` builds a fresh VSpace + loads ELF + applies `file_actions` (`open`/`close`/`dup2`) + `spawnattr` (`sigmask`, `pgroup`, `flags`) in one coordinated sequence without copying a single parent page table!
- **ELF64 & Script Loader (`execve`):**
  - Supports static ELF64, PIE ELF64, dynamic ELF64 (`PT_INTERP` loads `ld-musl-<arch>-fastix.so.1` alongside main binary), and `#!` shebang scripts (recursive depth limit = 4, optional single argument).
  - Validates ELF headers, program headers, segment bounds, non-overlapping segments, `p_filesz <= p_memsz`, integer overflows.
  - Handles `O_CLOEXEC` FD closing via a single async `VFS_EXEC_CLOEXEC` work item acknowledged downward by `vfsd`, signal handler reset (`SIG_IGN` preserved, caught handlers reset to `SIG_DFL`, pending signals preserved per POSIX), `setuid`/`setgid` bit handling + `AT_SECURE=1` (clearing `LD_PRELOAD`/`LD_LIBRARY_PATH` honored by musl).
- **Signals, Timers, Pipes & Futex:**
  - Full 64-signal mask (standard `1..31` + realtime `32..64` with queued `siginfo_t` up to `SIGQUEUE_MAX`).
  - `kill`, `tgkill`, `killpg`, `sigaction`, `sigprocmask`, `sigpending`, `sigsuspend`, `sigwait`, `sigwaitinfo`, `sigtimedwait`, `sigaltstack`, `signalfd`.
  - High-resolution timer wheel (`clock_nanosleep`, `nanosleep`, `alarm`, `setitimer`, POSIX `timer_create`/`timer_settime`, `timerfd`).
  - Fast SPSC shared-memory `pipe`/`pipe2` (atomic writes <= 4096 bytes `PIPE_BUF`, `SIGPIPE`/`EPIPE` on write to closed read-end, EOF on read from closed write-end, `O_NONBLOCK`, `FIONREAD`).
  - Userspace futex table (`FUTEX_WAIT`, `FUTEX_WAKE`, `FUTEX_REQUEUE`, `FUTEX_CMP_REQUEUE`, `FUTEX_WAIT_BITSET`, `FUTEX_WAKE_BITSET`, private & shared keys, robust list cleanup on thread exit).
  - `eventfd` (`EFD_CLOEXEC`, `EFD_NONBLOCK`, `EFD_SEMAPHORE`).
  - Resource limits (`RLIMIT_NOFILE`, `RLIMIT_AS`, `RLIMIT_DATA`, `RLIMIT_STACK`, `RLIMIT_NPROC`, `RLIMIT_FSIZE`, `RLIMIT_CORE`) and accounting (`getrusage`, `times`, `CLOCK_PROCESS_CPUTIME_ID`, `CLOCK_THREAD_CPUTIME_ID`).

#### C. `vfsd` (Virtual Filesystem, Unified Page Cache & Filesystems)
- **Unified FD Namespace & Cross-Server Routing:**
  - Every process's FD table (`fd_num -> struct open_file`) is managed consistently with O(1) lookup (`fd_table` array, dynamically growing up to `RLIMIT_NOFILE`).
  - Open file types: `FT_VNODE` (regular/dir/symlink), `FT_PIPE` (backed by `procd` ring), `FT_TTY` (backed by `ttyd`), `FT_SOCKET` (backed by `netd`), `FT_EVENTFD`/`FT_TIMERFD`/`FT_SIGNALFD`, `FT_EPOLL`.
  - When a process passes an FD in `read`/`write`/`ioctl`/`fcntl`/`close`/`dup`/`poll`, `libfastix_sys` routes directly to the owning server when the FD type is cached in a small userspace FD-type hint table (verified by the server on mismatch!) — **zero extra proxy hops** for sockets, pipes, and TTYs!
  - `close()` and `dup2()` keep `vfsd` authoritative for FD numbering and `O_CLOEXEC` flags, coordinating refcount drops cleanly.
- **VFS Core Semantics (All Subtle POSIX Edge Cases):**
  - **Path Resolution:** Handles absolute/relative/`*at(dirfd, ...)` paths, `.` and `..` (including `..` crossing mount boundaries and staying clamped at `/` or `chroot` root), symlink expansion with loop guard (`ELOOP` after 40 expansions), trailing slash enforcing directory (`ENOTDIR`/`EISDIR`), `O_NOFOLLOW`, `O_DIRECTORY`.
  - **Unlinked-but-Open Files:** Vnodes track `link_count` (on disk) AND `open_refcount` (in memory). `unlink()` removes the directory entry immediately; blocks/inode are freed ONLY when both `link_count == 0` AND `open_refcount == 0`.
  - **Atomic `rename()` / `renameat()`:** Atomic replacement within a filesystem; `EXDEV` across filesystems; rejects renaming a directory into its own subtree (`EINVAL`) or overwriting non-empty directory (`ENOTEMPTY`).
  - **File Locking:** Both POSIX record locks (`fcntl` `F_GETLK`/`F_SETLK`/`F_SETLKW`, byte-range, per-process release on *any* close of that file!) and BSD `flock()` (per open-file description) + OFD locks (`F_OFD_SETLK`). Deadlock detection returns `EDEADLK`.
  - **Sparse Files:** Seeking past EOF and writing creates holes; reading holes returns zeroed pages without allocating disk blocks; `ftruncate()` shrinks or extends with zero-fill.
  - **Permissions & Ownership:** Full `rwx` checks for owner/group/other + supplementary groups + `uid==0` override; directory sticky bit (`S_ISVTX` restricts unlink/rename to file owner/dir owner/root); directory `S_ISGID` group inheritance; `umask` masking on creation; `atime`/`mtime`/`ctime` updates with nanosecond precision (`utimensat`, `UTIME_NOW`, `UTIME_OMIT`).
- **Unified Page Cache:**
  - Hash/radix cache mapping `(mount_id, ino, page_index) -> cached_frame`.
  - Serves BOTH `read`/`write` AND `procd`'s `mmap` page faults from the exact same physical frame capabilities → **100% cache coherency between `read`/`write` and `mmap` with zero double-buffering!**
  - Dirty page tracking, sequential read-ahead (up to 128 KB), periodic writeback + explicit `fsync`/`fdatasync`/`sync`, Clock-Pro/LRU eviction under memory pressure.
- **Filesystem Backends:**
  1. `cpiofs`: Read-only/unpack-to-tmpfs boot initramfs (`newc` SVR4 CPIO format).
  2. `tmpfs`: Full-featured RAM filesystem (dirs, files, symlinks, hardlinks, sparse files, `mmap`, size quota, fast O(1) memory-backed page cache).
  3. `devfs`: Virtual `/dev` exposing `null`, `zero`, `full`, `random`, `urandom`, `tty`, `console`, `ptmx`, `pts/`, `shm/`, and block device nodes (`vda`, `vda1`, ...).
  4. `procfs`: `/proc/self`, `/proc/<pid>/{cmdline,environ,exe,cwd,fd/,maps,stat,status}`, `/proc/{mounts,meminfo,uptime,cpuinfo,loadavg,stat,version,kmsg}` — needed by `ps`, `top`, `free`, `df`, debuggers, and many C programs.
  5. `ext2` (Phase 4): Full read/write ext2 (1 KB / 4 KB blocks, direct + single/double/triple indirect blocks, fast symlinks, block/inode bitmaps, clean unmount flag).
  6. `ext4` (Phase 8): Extents, 64-bit block numbers, `htree` directory indexing, and `jbd2` metadata journaling for power-loss crash consistency.
  7. `fat32` (Phase 8): Long filenames (LFN), EFI/removable media interoperability.
  8. `virtio-fs` / `9p` (Optional dev accelerator): Mount host directory directly inside QEMU for instant edit-compile-test loops.

#### D. `devd` & Isolated Userspace Drivers
- **Least-Privilege Isolation:** Each hardware driver runs in its own seL4 protection domain with ONLY the specific MMIO page frames, DMA buffer frames, and `seL4_IRQHandler` capabilities for its own device.
- **Driver Supervision:** `devd` monitors driver fault endpoints. If a non-essential driver (e.g., `virtio_net`) crashes, `devd` logs the fault, revokes/re-initializes its capabilities, and restarts the driver cleanly without affecting `procd`, `vfsd`, or user processes!
- **Hardware Discovery:**
  - AArch64: Flattened Device Tree (FDT/DTB) parser in `devd` (`compatible` string matching for PL011, PL031 RTC, `virtio,mmio`, GIC).
  - x86_64: ACPI MADT/MCFG + PCIe configuration space enumeration + legacy COM1/PIT/CMOS.
- **Block & Network Driver Data Path:**
  - `virtio_blk`: Asynchronous virtqueue descriptor rings with scatter-gather DMA directly into `vfsd`'s page-cache physical frames (zero extra bounce copies!). Supports GPT/MBR partition parsing (`vda1`, `vda2`).
  - `virtio_net`: Pre-posted RX descriptor pool + TX completion ring shared directly with `netd` (zero-copy packet handoff).
  - `virtio_rng`: Seeds a fast userspace CSPRNG (ChaCha20) for `getrandom()`, `/dev/random`, `/dev/urandom`, and `AT_RANDOM` (non-blocking once seeded, never stalls boot).

#### E. `ttyd` (Console, Termios, Job Control & PTYs)
- Full POSIX terminal subsystem (`<termios.h>`):
  - **Input processing (`c_iflag`):** `IGNBRK`, `BRKINT`, `IGNPAR`, `ISTRIP`, `INLCR`, `IGNCR`, `ICRNL`, `IXON`, `IXOFF`, `IXANY`, `IUTF8`.
  - **Output processing (`c_oflag`):** `OPOST`, `ONLCR`, `OCRNL`, `ONOCR`, `ONLRET`, `TAB3`.
  - **Local modes (`c_lflag`):** `ISIG` (generates `SIGINT`/`SIGQUIT`/`SIGTSTP` to foreground `pgid` via `procd` notification), `ICANON` (line editing buffer: `VERASE`, `VKILL`, `VWERASE`, `VEOF`, `VEOL`), `ECHO`, `ECHOE`, `ECHOK`, `ECHONL`, `ECHOCTL`, `NOFLSH`, `TOSTOP` (`SIGTTOU` for background writes).
  - **Non-canonical reads:** Full `VMIN` / `VTIME` 4-case state machine (0/0, >0/0, 0/>0, >0/>0).
  - **Window size & `SIGWINCH`:** `TIOCGWINSZ`, `TIOCSWINSZ` (sends `SIGWINCH` to foreground `pgid`).
  - **PTY Subsystem:** `/dev/ptmx` allocates paired master + `/dev/pts/N` slave; supports `TIOCGPTN`, `TIOCSPTLCK`, `TIOCSCTTY`, `TIOCNOTTY`, `TIOCGPGRP`, `TIOCSPGRP`, `TCGETS`/`TCSETS`/`TCSETSW`/`TCSETSF`, `TCFLSH`, `TCSBRK`, `FIONREAD`, `POLLIN`/`POLLOUT`/`POLLHUP` when master or slave closes.

#### F. `netd` (Networking, Sockets & Unified Event Multiplexing)
- **`AF_UNIX` (Local Domain Sockets — Phase 7A):**
  - `SOCK_STREAM`, `SOCK_DGRAM`, `SOCK_SEQPACKET`, `socketpair()`.
  - Filesystem-bound sockets (coordinated with `vfsd` socket vnodes) and abstract namespace.
  - Ancillary data (`cmsg`): **`SCM_RIGHTS`** (FD passing across processes via `vfsd` ref transfer — essential for privilege-separated Unix programs like SSH, dbus, tmux, web servers!) and `SCM_CREDENTIALS` (`SO_PEERCRED`).
- **TCP/IPv4/IPv6 Stack (Phase 7B):**
  - Clean, modular C17 zero-copy packet engine (or tightly integrated minimal lwIP core wrapped in Fastix zero-copy rings — decision gate in Phase 7 based on footprint/throughput benchmarks; prefer custom lean C17 stack or stripped lwIP with zero-copy pbufs mapped directly over `virtio_net` DMA buffers).
  - Protocols: Ethernet, ARP, IPv4, ICMPv4, IPv6, NDP, ICMPv6, UDP, full RFC-compliant TCP (3-way handshake, slow start, congestion avoidance, fast retransmit/recovery, SACK, window scaling, keepalive, Nagle/`TCP_NODELAY`, `SO_REUSEADDR`/`SO_REUSEPORT`, `SO_RCVBUF`/`SO_SNDBUF`, `SO_LINGER`, `SO_RCVTIMEO`/`SO_SNDTIMEO`, `shutdown(RD/WR/RDWR)`, `MSG_PEEK`/`MSG_DONTWAIT`/`MSG_NOSIGNAL`), loopback `lo`, routing table.
  - Standard socket syscalls: `socket`, `bind`, `listen`, `accept`/`accept4`, `connect`, `getsockname`, `getpeername`, `sendto`, `recvfrom`, `sendmsg`, `recvmsg`, `setsockopt`, `getsockopt`, `shutdown`.
- **Unified Readiness Multiplexing (`poll`, `ppoll`, `select`, `pselect`, `epoll`):**
  - Crucial microkernel problem: a single `poll()`/`epoll_wait()` call often watches a **mix** of pipes (`procd`), TTYs (`ttyd`), sockets (`netd`), and regular files (`vfsd`).
  - **Fastix Unified Event Architecture:**
    - Every waitable kernel object across `procd`, `vfsd`, `ttyd`, and `netd` exposes a lightweight **Event Publisher** interface: when registered into a process's `epoll` set (or temporary `poll` set), the owning server signals a single per-process/per-epoll `seL4_Notification` capability with a badge bit whenever readiness transitions!
    - Regular files are always immediately ready (per POSIX).
    - **Result:** `poll`/`select`/`epoll` works seamlessly across ALL FD types simultaneously with O(1) wakeup latency and zero polling loops!

### 6.4 File Descriptor Ownership & Cross-Server Consistency Protocol
FD state is split across servers: `vfsd` owns FD *numbers*, while `procd` (pipes, eventfd, timerfd, signalfd), `ttyd` (TTYs/PTYs), and `netd` (sockets, epoll) own the *objects* behind them. This is the most bug-prone part of the design, so it follows these exact rules (wire formats in `vfs_proto.h`):

1. **Single Authority:** `vfsd` alone allocates FD numbers, stores `FD_CLOEXEC`, and owns every **open file description (OFD)**: offset, status flags (`O_APPEND`, `O_NONBLOCK`, ...), and the OFD refcount. An OFD for a non-vnode object stores an opaque `struct fastix_obj_ref { uint16_t owner; uint16_t type; uint32_t gen; uint64_t obj_id; }` minted by the owning server. Owner servers count references *per OFD*, never per FD or per process.
2. **Install (create an FD for a new object):** The creating server installs the FD, so the client can never forge an `obj_ref`:
   - `netd`/`ttyd` (Rank 40) make a downward `seL4_Call` to `vfsd` (`VFS_FD_INSTALL(client, obj_ref, flags)`) and return the FD number to the client in the same reply.
   - `procd` (Rank 10, never calls upward) parks the client, enqueues `VFS_FD_INSTALL` on its async `procd -> vfsd` ring, and replies to the client when `vfsd`'s downward completion call arrives with the FD number(s) (e.g. both ends of `pipe2`).
3. **FD Generation Table (Zero-IPC Validation):** `vfsd` keeps a per-process array `uint32_t fd_gen[fd]`, bumped whenever an FD slot is closed or reused (`close`, `dup2`/`dup3` over an open FD, `exec` close-on-exec). The array lives in one page per 1024 FDs, written by `vfsd` and mapped **read-only** into `procd`, `ttyd`, and `netd`. Each owner server keeps a per-client mirror `fd -> (gen, obj)`; on every direct fastpath request it compares `mirror.gen` with `fd_gen[fd]` (one load, no IPC). On mismatch or miss: `netd`/`ttyd` refill the mirror with a downward `VFS_FD_LOOKUP` call; `procd` replies with an internal `-FASTIX_ESTALEFD` code and `libfastix_sys` re-routes that one call through `vfsd`. `-FASTIX_ESTALEFD` is never visible to applications.
4. **dup / dup2 / dup3 / `F_DUPFD`:** Handled entirely in `vfsd` (OFD refcount +1, new FD number, `FD_CLOEXEC` cleared on the new FD per POSIX). Owner servers learn about the new number lazily through rule 3. `dup2(fd, fd)` is a validated no-op.
5. **close:** `vfsd` removes the FD and bumps `fd_gen` **before** replying, so after `close()` returns, any direct-path use of that FD number in any thread fails validation and ends in `EBADF`. When an OFD refcount reaches 0, `vfsd` tells the owner: a downward `seL4_Call` to `procd` (`PROC_OBJ_RELEASE`), or an async ring entry + `seL4_Signal` upward to `ttyd`/`netd`. Owners free an object ONLY on this release message, which rules out double-frees and use-after-free between servers.
6. **fork / exec / exit:** `VFS_FORK_DUP`, `VFS_EXEC_CLOEXEC`, and `VFS_PROC_EXIT` (async from `procd`, acknowledged downward by `vfsd`) are the ONLY paths that bulk-copy or bulk-close FD tables. Their acknowledgement lists the released `procd`-owned objects, so `procd` unmaps pipe ring frames from that VSpace and frees the matching `CAP_DYN_RINGS` slots in the same step. On `fork`, `procd` copies the ring mappings (as `MAP_SHARED` VMAs) and ring notification caps into the child at identical slots and addresses, so inherited userspace ring fastpaths work in the child unchanged.
7. **Userspace Pipe Fastpath Safety:** `libfastix_sys` keeps its own FD -> ring table and removes the entry *before* sending `close`, so the calling process stops using a ring before its FD disappears. Ring frames are only unmapped/freed after the owner receives the OFD release in rule 5, so no other process can ever see a freed ring.
8. **`SCM_RIGHTS` in flight:** On `sendmsg`, `netd` calls `VFS_OFD_PIN(sender, fds[])`, which takes an extra OFD reference per FD and returns opaque in-flight handles. On `recvmsg`, it calls `VFS_FD_INSTALL_PINNED(receiver, handle, MSG_CMSG_CLOEXEC?)`. If a socket is destroyed with handles still queued, `netd` calls `VFS_OFD_UNPIN`. Reference cycles (an `AF_UNIX` socket queued inside itself) are bounded by `FASTIX_SCM_MAX_FD = 253` per message and `FASTIX_UNIX_INFLIGHT_MAX` per user, and collected by a small mark-and-sweep pass over in-flight `AF_UNIX` sockets in `netd` (runs only when in-flight count > 0).
9. **Required Tests (Phase 4–7):** a 100+ seed randomized model test that runs `open/pipe2/socket/dup/dup2/dup3/close/fork/exec/exit/SCM_RIGHTS` sequences across 1–8 threads and checks after every step that the FD table, OFD refcounts, owner objects, and ring mappings match a reference model exactly, with 0 leaked objects at the end.

---

## 7. Standard C Library (`libc`) & Dynamic Linker Plan

### 7.1 Architectural Decision: `musl libc` (Userland) + `libos` (OS Servers)
Why this dual-layer approach is the optimal senior-engineering solution to *"small fast libc library that can run any C program or make your own"*:

| Requirement | Writing a Full POSIX Libc From Scratch | Porting `musl libc` to Fastix (+ `libos` for servers) |
| :--- | :--- | :--- |
| **Run ANY standard C program** | Years of subtle bugs in `printf` float formatting, `libm` ULP accuracy, `regex`, `iconv`, `locale`, `pthread` cancellation, DNS, `dlopen`. | **Immediate, battle-tested ISO C11/C17/C23 + POSIX.1 compliance**; GCC, Clang, SQLite, Python, Git, Lua, Curl compile cleanly. |
| **Size & Speed** | Can be tiny, but loses features or grows to musl's size once complete. | **Already tiny & blazingly fast**: ~16–24 KB static hello, ~600 KB full shared `libc.so + ld.so`, zero bloat, clean C, MIT license. |
| **Server Minimalism** | Clean for servers. | **`libos`** gives OS servers a tiny (~20 KB), freestanding, zero-POSIX runtime. |
| **ABI Ownership** | Full control. | **Full control**: Fastix defines its own clean native syscall/IPC ABI; `musl` calls into `libfastix_sys` via thin `__syscall0..6` wrappers. |

*(Optional Future Track: Because the Fastix Syscall ABI in `include/fastix/abi.h` is clean and libc-agnostic, an ultra-minimal custom `libfastixc` subset can optionally be built later for tiny single-purpose appliances without changing a single line of any OS server.)*

### 7.2 Exact `musl` Porting Blueprint (`libc/musl_port/`)
1. **Architecture Syscall Layer (`arch/aarch64/syscall_arch.h` & `arch/x86_64/syscall_arch.h`):**
   - Replace Linux `svc #0` / `syscall` instructions with calls into `libfastix_sys` (`__fastix_syscall0` .. `__fastix_syscall6`).
   - Inside `libfastix_sys`:
     - **Tier-0 VDSO Fastpath:** Intercept `SYS_getpid`, `SYS_getppid`, `SYS_getuid`, `SYS_geteuid`, `SYS_getgid`, `SYS_getegid`, `SYS_getpgid(0)`, `SYS_clock_gettime` directly from VDSO/procinfo pages (0 IPCs).
     - **Tier-1 Userspace Ring Fastpath:** Intercept `read`/`write` on SPSC pipe/ring FDs directly in userspace when data/space is available (0 IPCs).
     - **Tier-2 Direct Server Fastpath:** Route FD syscalls directly to `CAP_VFSD_EP`, `CAP_NETD_EP`, `CAP_TTYD_EP`, or `CAP_PROCD_EP` using the local FD-owner cache.
     - **Tier-3 Standard `procd` IPC:** Process, VM, signal, futex, and timer calls via `seL4_Call(CAP_PROCD_EP, ...)`.
2. **Thread Creation (`__clone` / `pthread_create`):**
   - Map musl's internal `__clone` directly to `SYS_fastix_thread_create(entry, stack, flags, arg, ptid, tls, ctid)` in `procd`.
   - `procd` allocates TCB + SC + IPC buffer + notification, sets TLS base, handles `CLONE_CHILD_CLEARTID` (writes `0` + `FUTEX_WAKE` on exit for `pthread_join`).
3. **Dynamic Linker (`ld-musl-aarch64-fastix.so.1`):**
   - musl's `libc.so` IS its own dynamic linker (`PT_INTERP = /lib/ld-musl-aarch64-fastix.so.1`).
   - Requires `procd` ELF loader to pass `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_BASE`, `AT_ENTRY`, `AT_PAGESZ`, `AT_RANDOM` accurately on the initial stack.
   - Enables shared libraries, `dlopen`/`dlsym`/`dlclose`, plugin architectures, and massive memory savings when running many dynamic binaries.
4. **Cross-Compiler Sysroot & `fastix-cc`:**
   - Produce a self-contained sysroot (`build/sysroot/usr/{include,lib}`) with `crt1.o`, `Scrt1.o`, `crti.o`, `crtn.o`, `libc.a`, `libc.so`, `libm.a`, `libpthread.a`, `libcompiler_rt.a`.
   - Provide `fastix-cc` (wrapper around Clang + `ld.lld` + `--sysroot`) supporting both static (`-static`) and dynamic/PIE builds so any external C project builds with `CC=fastix-cc`.

### 7.3 Complete Fastix Native Syscall ABI Map (`include/fastix/abi.h`)
All syscalls use clean, modern 64-bit semantics (64-bit `off_t`, 64-bit `time_t` — Year-2038 safe from Day 1, 64-bit `ino_t`, `*at` variants as primaries, no legacy 32-bit cruft):

| Group | Syscall IDs | Syscalls Included | Primary Server |
| :--- | :--- | :--- | :--- |
| **0x000 Process & Thread** | `0..63` | `exit`, `exit_group`, `fork`, `vfork`, `posix_spawn_fast`, `execve`, `execveat`, `wait4`, `waitid`, `thread_create`, `set_tid_address`, `set_tls`, `getpid`*, `getppid`*, `gettid`, `getpgid`*, `setpgid`, `getsid`, `setsid`, `getuid`*, `geteuid`*, `getgid`*, `getegid`*, `setuid`, `seteuid`, `setreuid`, `setresuid`, `getresuid`, `setgid`, `setegid`, `setregid`, `setresgid`, `getresgid`, `getgroups`, `setgroups`, `umask`, `getrlimit`, `setrlimit`, `prlimit64`, `getrusage`, `times` | `procd` (* = VDSO) |
| **0x040 Memory** | `64..79` | `brk`, `mmap`, `munmap`, `mprotect`, `mremap`, `msync`, `madvise`, `mincore`, `mlock`, `munlock` | `procd` |
| **0x050 Signals** | `80..99` | `rt_sigaction`, `rt_sigprocmask`, `rt_sigpending`, `rt_sigsuspend`, `rt_sigtimedwait`, `rt_sigreturn`, `sigaltstack`, `kill`, `tkill`, `tgkill`, `rt_sigqueueinfo`, `signalfd4` | `procd` |
| **0x070 Time & Sched** | `112..135` | `clock_gettime`*, `clock_settime`, `clock_getres`, `clock_nanosleep`, `nanosleep`, `setitimer`, `getitimer`, `timer_create`, `timer_settime`, `timer_gettime`, `timer_getoverrun`, `timer_delete`, `timerfd_create`, `timerfd_settime`, `timerfd_gettime`, `sched_yield`, `sched_getparam`, `sched_setparam`, `sched_getscheduler`, `sched_setscheduler`, `sched_getaffinity`, `sched_setaffinity`, `getpriority`, `setpriority` | `procd` (* = VDSO) |
| **0x090 Sync & Local IPC** | `144..169` | `futex`, `set_robust_list`, `get_robust_list`, `pipe2`, `eventfd2`, `shm_open_fast`, `mq_open`, `mq_unlink`, `mq_timedsend`, `mq_timedreceive`, `mq_getsetattr`, `semget`, `semop`, `semctl`, `shmget`, `shmat`, `shmdt`, `shmctl` | `procd` |
| **0x0B0 Filesystem & FD** | `176..239` | `openat`, `close`, `read`, `write`, `pread64`, `pwrite64`, `readv`, `writev`, `preadv`, `pwritev`, `lseek`, `dup`, `dup3`, `fcntl`, `ioctl`, `flock`, `fsync`, `fdatasync`, `sync`, `syncfs`, `truncate`, `ftruncate`, `fallocate`, `fstat`, `newfstatat`, `statfs`, `fstatfs`, `faccessat`, `fchmod`, `fchmodat`, `fchown`, `fchownat`, `utimensat`, `mkdirat`, `mknodat`, `unlinkat`, `renameat2`, `linkat`, `symlinkat`, `readlinkat`, `getdents64`, `getcwd`, `chdir`, `fchdir`, `chroot`, `mount`, `umount2`, `pivot_root`, `sendfile`, `copy_file_range` | `vfsd` (+ direct TTY/Net/Pipe routing) |
| **0x100 Sockets & Events** | `256..295` | `socket`, `socketpair`, `bind`, `listen`, `accept4`, `connect`, `getsockname`, `getpeername`, `sendto`, `recvfrom`, `sendmsg`, `recvmsg`, `setsockopt`, `getsockopt`, `shutdown`, `ppoll`, `pselect6`, `epoll_create1`, `epoll_ctl`, `epoll_pwait` | `netd` + Unified Event Broker |
| **0x130 System & Misc** | `304..325` | `uname`, `sethostname`, `setdomainname`, `getrandom`, `sysinfo`, `reboot`, `syslog`/`kmsg`, `prctl`, `arch_prctl` | `procd` |

---

## 8. Small, Fast C Userland Plan (Reuse Proven Code, Optimize the OS)

### 8.1 Core Design Decision: Reuse, Don't Rewrite
The userland is built from small, proven, permissively licensed upstream C projects. Fastix-authored code is limited to `init` and tiny glue tools. Rationale:
- A correct POSIX shell, `awk`, `sed`, `find`, and `tar` are each multi-month projects with endless edge cases; upstream versions already have years of real-world testing (toybox ships on Android).
- Upstream programs are **real POSIX clients**: every bug they hit on Fastix exposes a real bug in the libc port or OS servers, which is exactly the testing Fastix needs.
- A Unix system feels powerful because it runs real software (`make`, `git`, `vim`, `ssh`, compilers), not because of who wrote `ls`. That power comes from the correctness of `procd`/`vfsd`/`ttyd`/`netd` + musl, which is where Fastix's effort goes.

**Base Userland Stack (all static, built with `fastix-cc` against Fastix musl):**

| Role | Component | License | Installed As |
| :--- | :--- | :--- | :--- |
| Core tools (multicall) | **toybox** | 0BSD | `/bin/toybox` + symlinks (`/bin/ls`, `/bin/cat`, ...) |
| POSIX shell | **dash** | BSD | `/bin/sh` |
| `awk` | **onetrue-awk** (BWK awk) | BWK/MIT-style | `/usr/bin/awk` (unless toybox `awk` passes Section 8.4 tests) |
| PID 1 | **Fastix `init`** (Section 8.2) | Fastix | `/sbin/init` |
| Gap fillers | `userland/extra/` (Fastix-authored, tiny) | Fastix | Only for tools no permissive upstream provides acceptably |

**Licensing Policy:** The base image contains ONLY permissively licensed code (0BSD, BSD, MIT, ISC, and similar). GPL code (BusyBox, GNU coreutils, bash, GNU make, ...) may exist ONLY as optional ports in `userland/ports/`, never in the base image.

**Upstream Discipline:**
- Every upstream is pinned to an exact commit/release in `third_party/`.
- Fastix patches live in `userland/<pkg>/patches/` as small, numbered, documented patch files. Each patch is either (a) a portability fix that is upstreamable, or (b) a measured performance change backed by a `bench/` result. Fixes for Fastix OS bugs go in the OS, NEVER as userland workarounds.
- Upgrading an upstream = bump the pin, re-apply patches, rerun Section 8.4 tests.

### 8.2 PID 1 (`/sbin/init`)
- Tiny (< 15 KB static) Fastix-authored C17 supervisor:
  1. Sets safe environment (`PATH=/bin:/sbin:/usr/bin:/usr/sbin`, `HOME=/root`, `TERM=vt100`).
  2. Mounts `/proc`, `/dev`, `/tmp`, `/dev/pts` if not already mounted.
  3. Executes `/etc/rc` with `/bin/sh` and waits for completion.
  4. Spawns interactive `/bin/sh` on `/dev/console` (and optional services declared in `/etc/inittab`).
  5. Loops in `waitpid(-1, ...)` reaping orphaned zombies forever.
  6. On `SIGTERM`/`SIGUSR1`/`SIGUSR2` (from `poweroff`/`reboot`/`halt`): sends `SIGTERM` to all processes, waits up to 3 s, sends `SIGKILL`, calls `sync()`, unmounts filesystems read-only, invokes `reboot(FASTIX_REBOOT_POWEROFF|RESTART)`.

### 8.3 Shell (`dash` as `/bin/sh`) & Toybox Configuration
- **`dash`** is the system shell: small, fast to start, POSIX-focused, and used as `/bin/sh` by major Linux distributions, so most shell scripts already run on it.
  - Interactive job control (`fg`, `bg`, `jobs`, Ctrl-Z) exercises `ttyd` + `procd` process groups exactly as real software does.
  - **Measured spawn optimization:** dash launches external commands with `fork()` (some versions use `vfork()` for simple commands; check at the pinned version). Fastix first makes `fork()` itself fast (Section 10.1). Only if `bench/macro` shell loops show a real gap does Fastix add a small, upstreamable patch using `vfork()`/`posix_spawn()` for simple commands.
  - An interactive line editor (history, arrow keys) is optional: dash's `libedit` support if available, otherwise users run a richer shell port (e.g. `mksh`) from `userland/ports/` interactively while scripts keep using `dash`.
- **`toybox`** is built from a checked-in `userland/toybox/fastix.config`:
  - Commands are enabled from an explicit **allowlist**. Commands in toybox's `pending/` directory are enabled ONLY after they pass the Section 8.4 tests on Fastix; otherwise the tool comes from another permissive upstream or `userland/extra/`.
  - Toybox's own shell (`toysh`) is NOT used as `/bin/sh` until it is complete upstream and passes the shell test suite; `dash` remains `/bin/sh`.
  - Built `-Os -static` with `--gc-sections`; the size budget in Section 1.4 is enforced in CI.

### 8.4 Utility Inventory & Source of Each Tool
The required command set is unchanged; what changes is where each tool comes from. Exact per-command sources are tracked in `userland/INVENTORY.md` (command → upstream → status → test file) and verified at the pinned commits.

| Category | Utilities | Primary Source |
| :--- | :--- | :--- |
| **Filesystem & File Ops** | `ls`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `ln`, `chmod`, `chown`, `chgrp`, `touch`, `stat`, `readlink`, `realpath`, `basename`, `dirname`, `pwd`, `mktemp`, `mkfifo`, `mknod`, `dd`, `df`, `du`, `find`, `sync`, `truncate`, `install` | toybox |
| **Text & Stream Processing** | `cat`, `tac`, `head`, `tail` (incl. `-f`), `tee`, `wc`, `cut`, `paste`, `tr`, `sort`, `uniq`, `grep` (`-E`, `-F`, `-r`, `-i`, `-v`, `-n`, `-c`), `sed` (POSIX BRE/ERE), `diff`, `cmp`, `comm`, `fold`, `fmt`, `expand`, `unexpand`, `nl`, `od`, `hexdump`, `strings`, `printf`, `echo`, `expr`, `seq`, `yes`, `split`, `join`, `xargs` | toybox |
| **`awk`** | POSIX `awk` | onetrue-awk |
| **Process & System Control** | `ps`, `top`, `kill`, `killall`, `nice`, `renice`, `nohup`, `timeout`, `env`, `printenv`, `id`, `whoami`, `groups`, `uname`, `hostname`, `uptime`, `free`, `date`, `sleep`, `time`, `watch`, `dmesg`, `mount`, `umount`, `chroot`, `stty`, `tty`, `clear`, `reset`, `login`, `su`, `passwd` | toybox |
| **Power Control** | `reboot`, `poweroff`, `halt` | toybox if it works with Fastix `init` signals; otherwise `userland/extra/` |
| **Archiving & Integrity** | `tar` (ustar/pax), `cpio`, `gzip`/`gunzip`/`zcat`, `cksum`, `md5sum`, `sha256sum`, `base64` | toybox |
| **Networking** | `ifconfig`/`route` or `ip`, `ping`, `nc` (netcat), DHCP client, `wget` (HTTP/1.1), `host`/`nslookup` | toybox (some may be in `pending/`; else ports or `extra/`) |
| **Text Editor** | `vi` | toybox `vi` if it passes tests; full `vim`/`nano` available as ports |

**Userland Test Requirements:**
- Run **toybox's own test suite** (`tests/*.test`) inside Fastix; any test that cannot run because the harness needs a non-POSIX shell feature is run on the host against the same pinned toybox, and the Fastix result must match the host result.
- Run a **POSIX shell conformance corpus** against `dash` on Fastix and compare output byte-for-byte with `dash` built from the same pin on the host (macOS/Linux). Any difference is a Fastix OS/libc bug until proven otherwise.
- Run `onetrue-awk`'s regression tests (`REGRESS`) on Fastix and compare with host results.
- **Differential Rule:** Because the same upstream source runs on both host and Fastix, every userland test is a differential test. Output must be identical; a mismatch points directly at Fastix.

### 8.5 Optional Fastix-Native Replacements (Post-v1.0, Benchmark-Gated)
Fastix-authored replacements (a custom multicall `fastix-box` or a custom shell `fsh`) are NOT part of the plan to v1.0. A replacement for any individual tool may be written later ONLY if:
1. A `bench/` measurement shows the upstream tool is a real bottleneck on Fastix that OS/libc tuning cannot fix, and
2. The replacement passes the exact same Section 8.4 tests and differential comparisons as the tool it replaces, and
3. It stays within the size budget and is recorded in an ADR.

Speed techniques worth applying via small upstreamable patches or replacements: 64 KB aligned I/O buffers (`FASTIX_IO_BUFSIZE`) in `cat`/`cp`/`dd`; `copy_file_range`/`sendfile` in `cp` and `cat`; `vfork`/`posix_spawn` in the shell and `xargs`.

### 8.6 Third-Party C Program Proof Suite & Self-Hosting (`userland/ports/`)
To prove beyond doubt that Fastix runs real-world arbitrary C programs:
1. **Tier A (Phase 6/7):** `lua`, `sqlite3`, `zlib`, `coremark`, `dhrystone`.
2. **Tier B (Phase 8/9):** `gmake` / `bmake`, `dropbear` (SSH server + client over PTYs + TCP!), `curl` (or `mbedtls`), `mksh`, `vim`/`nano`, `git`.
3. **Tier C (Self-Hosting Milestone — Phase 9):** Port **TinyCC (`tcc`)** first (fast native C compiler inside Fastix in < 300 KB), which compiles C programs directly inside Fastix (Phase 9 proof: rebuild `dash`, `onetrue-awk`, and Fastix `init` from source inside Fastix). A native **Clang/LLVM** or **GCC** is a post-v1.0 stretch goal: it needs C++ runtime support (`libc++`/`libstdc++`, exceptions, unwinding), several GB of RAM for its own build, and dynamic loading at scale. It is NOT an exit criterion for any phase.

---

## 9. Complete POSIX Compliance Matrix & Edge-Case Checklist

Toy Unix systems fail POSIX in predictable, subtle places. Fastix explicitly tracks and tests EVERY one of these requirements:

| Subsystem | Mandatory POSIX Headers | Critical Edge Cases That MUST Pass |
| :--- | :--- | :--- |
| **Processes & Signals** | `<unistd.h>`, `<sys/wait.h>`, `<signal.h>`, `<spawn.h>` | `SIGCHLD` with `SA_NOCLDWAIT` auto-reaps zombies; `SA_RESTART` restarts `read`/`write`/`waitpid` but NEVER restarts `nanosleep`/`poll`/`select`/`sigsuspend`; orphaned process group with stopped member gets `SIGHUP` then `SIGCONT`; `fork()` in multithreaded process leaves ONLY calling thread alive in child; `exec()` preserves `SIG_IGN` and resets caught handlers; `SIGKILL`/`SIGSTOP` cannot be caught, blocked, or ignored. |
| **Filesystem & VFS** | `<fcntl.h>`, `<sys/stat.h>`, `<dirent.h>`, `<sys/statvfs.h>` | Unlinked open files remain readable/writable until last FD closes; `O_APPEND` writes are atomic across concurrent processes; `O_CREAT \| O_EXCL` is atomic and fails on dangling symlinks; `pread`/`pwrite` do NOT alter file offset; `dup`/`dup2` share file offset & status flags (`O_NONBLOCK`) but NOT `FD_CLOEXEC`; `dup2(fd, fd)` succeeds without clearing `FD_CLOEXEC`; `rename("a", "a")` is a no-op returning 0; `link()` to directory fails (`EPERM`); sparse file holes read as zeroes; symlink loops return `ELOOP`. |
| **Memory Mapping** | `<sys/mman.h>` | `MAP_PRIVATE` writes never modify underlying file; `MAP_SHARED` writes are immediately visible to `read()` and other `MAP_SHARED` mappings; access past file EOF page triggers `SIGBUS`; partial `munmap()` / `mprotect()` splits VMAs cleanly; `fork()` preserves `MAP_SHARED` sharing and COW-isolates `MAP_PRIVATE`. |
| **Threads & Sync** | `<pthread.h>`, `<semaphore.h>`, `<stdatomic.h>`, `<threads.h>` | `pthread_cancel` deferred & async cancellation points; `pthread_cleanup_push/pop`; `pthread_key_create` destructor loops (`PTHREAD_DESTRUCTOR_ITERATIONS`); `pthread_once` thread-safe; `PTHREAD_MUTEX_RECURSIVE` & `PTHREAD_MUTEX_ERRORCHECK` & `PTHREAD_MUTEX_ROBUST`; `pthread_cond_timedwait` with `CLOCK_MONOTONIC` (`pthread_condattr_setclock`); `pthread_atfork` handlers. |
| **Terminals & PTYs** | `<termios.h>`, `<sys/ioctl.h>` | Canonical line editing (`ERASE`, `KILL`, `EOF` returning 0 bytes on empty line); `VMIN`/`VTIME` all 4 combinations; background process reading controlling TTY receives `SIGTTIN`, writing with `TOSTOP` receives `SIGTTOU`; PTY master close delivers `SIGHUP` + `POLLHUP` to slave. |
| **Sockets & IPC** | `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<netdb.h>`, `<sys/un.h>`, `<poll.h>`, `<sys/select.h>`, `<mqueue.h>`, `<sys/shm.h>` | `MSG_NOSIGNAL` suppresses `SIGPIPE`; `SO_ERROR` cleared on `getsockopt`; non-blocking `connect()` returns `EINPROGRESS` and signals writable on completion; `AF_UNIX` `SCM_RIGHTS` transfers open FDs across processes with proper refcount; `poll()` with `fd < 0` ignored cleanly (`revents = 0`); `select()` timeout `0` polls immediately, `NULL` blocks indefinitely. |
| **Time & Clocks** | `<time.h>`, `<sys/time.h>` | `CLOCK_MONOTONIC` never jumps backwards even when `CLOCK_REALTIME` is adjusted; `nanosleep` fills `rem` on `EINTR`; `clock_nanosleep(TIMER_ABSTIME)` sleeps to absolute deadline without drift; 64-bit `time_t` works cleanly past 2038. |
| **C Library & Localization** | `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>`, `<regex.h>`, `<glob.h>`, `<fnmatch.h>`, `<iconv.h>`, `<dlfcn.h>` | Full IEEE-754 `printf`/`strtod`, UTF-8 multibyte (`mbrtowc`/`wcrtomb`), POSIX BRE/ERE regex, `glob`/`fnmatch`, `getopt`/`getopt_long`, `setjmp`/`sigsetjmp`, `dlopen`/`dlsym`/`RTLD_NEXT`, `qsort_r`, `mkstemp`. |

---

## 10. Performance Engineering & Footprint Optimization Plan

Fastix treats speed and size as **first-class architectural invariants**, not afterthoughts.

### 10.1 Hot-Path Optimization Playbook
1. **Eliminate IPCs Where Safe:**
   - VDSO for read-only identity & clocks (`getpid`, `clock_gettime`).
   - Userspace atomic fastpath for uncontended futexes/mutexes/semaphores.
   - Userspace SPSC ring fastpath for pipes and Unix stream sockets.
   - Userspace FD-type routing cache so `read`/`write`/`ioctl`/`send`/`recv` hit the owning server directly in 1 hop.
2. **Stay on seL4's Verified IPC Fastpath:**
   - Measure fastpath hit-rate in debug builds (verify `seL4_Call` + `seL4_ReplyRecv` uses `<= 4` MRs and `0` extraCaps on common syscalls).
   - Keep hot IPC dispatch tables compact (array-indexed switch, branch-predicted, hot handlers in contiguous `.text.hot` section).
3. **Eliminate Memory Copies:**
   - Shared frame capabilities between `vfsd` page cache and `procd` VSpaces (`mmap` = 0 copies).
   - Direct DMA between `virtio_blk` and `vfsd` page-cache frames (0 bounce copies).
   - Direct DMA between `virtio_net` and `netd` packet rings (0 bounce copies).
   - `sendfile` / `copy_file_range` moves page-cache references inside `vfsd` without touching userland.
4. **Eliminate Allocation & Retype Overhead:**
   - Warm pools of TCBs, SCs, Replies, Notifications, CNodes, Page Tables, and pre-zeroed 4 KB frames in `procd`.
   - 2 MB superpages for large VMAs, `vfsd` page cache slabs, and server heaps (dramatically reduces TLB misses and page-table walks).
   - Cache-line alignment (`alignas(64)`) for per-CPU structures, lock heads, and ring producer/consumer indices.
5. **Fast Process Creation:**
   - `posix_spawn` and `vfork` used in `make` and `xargs`, and in `dash` via a measured patch only if needed (Section 8.3).
   - Sparse VSpace cloning during `fork()` (only traverse populated page-table subtrees; never scan empty regions).
6. **Binary & Memory Footprint Discipline:**
   - `-Os` + `-flto=thin` + `--gc-sections` + stripped symbols (keep unstripped `.sym` files in `build/symbols/` for GDB/backtraces).
   - Multicall `toybox` userland (one binary shared in the page cache for every core tool).
   - Static size-gate check in CI: fail build if any server, `toybox`, `dash`, or `awk` grows beyond its section budget without an explicit ADR.

### 10.2 Benchmark Suite (`bench/`)
Every optimization MUST be backed by before/after numbers from automated benchmarks:
- **Microbenchmarks (`bench/micro/`):** `bench_vdso`, `bench_null_ipc`, `bench_pipe_lat`, `bench_pipe_bw`, `bench_futex`, `bench_fork_exit`, `bench_spawn_true`, `bench_pagefault_cow`, `bench_mmap_touch`, `bench_stat_open_close`, `bench_file_seq_rw`, `bench_unix_sock`, `bench_tcp_loopback`, `bench_epoll_scale` (1000 FDs).
- **Macrobenchmarks (`bench/macro/`):** POSIX shell loop running 1000 commands, untarring a C source tree, compiling a multi-file C program with `tcc`, SQLite 10,000-row insert/query benchmark.
- **Measurement Honesty Rule:** Always state clearly whether benchmark numbers were collected on **QEMU TCG** (emulated, useful for relative regressions/instruction counts), **QEMU HVF/KVM** (near-native VM), or **real hardware** (authoritative cycle latency).

---

## 11. Verification, Testing, Fuzzing & Debugging Strategy

### 11.1 Automated Headless QEMU Verification Harness (`tests/run_qemu_tests.py`)
- Boots `fastix-image.elf` headlessly in `qemu-system-aarch64` (`-nographic -serial stdio -monitor none`) with a strict watchdog timeout (e.g., 60 s per suite).
- Parses structured test markers (`[FASTIX-TEST PASS]`, `[FASTIX-TEST FAIL]`, `[FASTIX-SUITE DONE rc=N]`) and terminates QEMU cleanly via poweroff/semihosting exit code.
- Zero manual interaction required — every phase's tests run with a single command (`ninja check` / `python3 tests/run_qemu_tests.py`).

### 11.2 Four-Tier Test Pyramid

| Tier | Scope | Tooling & Requirements |
| :--- | :--- | :--- |
| **Tier 1: Host Unit & Randomized Tests** | `libos` allocators, interval/radix trees, SPSC rings, ext2/ext4 parser, DTB parser, ELF validator, `init`; host builds of pinned `toybox`/`dash`/`onetrue-awk` as the differential reference for Section 8.4 | Native host compiler with `-fsanitize=address,undefined`; **100+ randomized seeds** per stateful data structure checking invariants (no leaks, exact model equivalence). |
| **Tier 2: Kernel/IPC & Leak Tests (QEMU)** | Untyped allocator, capability lifecycle, `procd` fault/COW/signal delivery, server crash isolation | Run 1,000× `fork+exec+exit` and 1,000× `mmap+fault+munmap` cycles; assert `free_untyped_bytes_after == free_untyped_bytes_before` (0-byte leak tolerance!). |
| **Tier 3: Behavioral POSIX Conformance (QEMU)** | Full POSIX surface | 1. **`libc-test`** (musl's official conformance/regression suite)<br>2. **Open POSIX Test Suite (OPTS)** (threads, signals, timers, semaphores, mqueues)<br>3. **`pjdfstest`** (VFS permissions, symlinks, unlink, rename, truncate, hardlinks)<br>4. **Fastix Behavioral Suite** (PTY job control, `SCM_RIGHTS`, `epoll` mixed FDs, COW + `mmap` coherency, `EINTR`/`SA_RESTART`, shell POSIX test vectors). |
| **Tier 4: Chaos, Stress & Syscall Fuzzing (QEMU)** | Reliability & security | Randomized syscall fuzzer (100+ seeds passing invalid pointers, huge lengths, invalid FDs, random flags — assert 0 server crashes); OOM stress test; `RLIMIT_NPROC` fork-bomb containment; concurrent multi-thread `fork`+signal+`mmap` torture; kill `virtio_net` driver mid-transfer and verify `devd` restarts it cleanly. |

### 11.3 Debugging Infrastructure
- **QEMU GDB Stub:** `tools/debug_qemu.sh` launches QEMU paused (`-s -S`) and attaches `lldb`/`gdb-multiarch` with symbols for `procd`, `vfsd`, `netd`, `ttyd`, and userland ELFs.
- **Fault Backtrace Printer:** On unhandled user or server fault, `procd` prints faulting PID/TID, PC, LR, SP, fault address, FSR, register dump, and VMA map (with offset inside ELF so `llvm-addr2line` resolves exact line numbers).
- **IPC Flight Recorder:** Compile-time optional ring buffer in `procd`/`vfsd` recording the last 4096 `(timestamp, pid, op, arg0, ret)` entries for diagnosing deadlocks and race conditions without distorting timing via serial prints.

---

## 12. Phased Implementation Roadmap (Phase 0 → Phase 10)

No phase may be declared complete until **ALL** its exit criteria pass and are verified with concrete test evidence.

### Phase 0: Toolchain, Build System & seL4 Hello World
- **Objectives:** Reproducible cross-build on macOS (Clang + `ld.lld` + Python venv for seL4) and booting unmodified upstream seL4 (MCS, AArch64 `qemu-arm-virt`) to serial output in headless QEMU.
- **Deliverables:**
  - Repo skeleton, pinned `third_party/seL4` and `third_party/seL4_tools`, `CMakeLists.txt`, `config/aarch64-qemu-virt.cmake`.
  - Frozen initial headers in `include/fastix/{config,abi,cspace,vspace,errno}.h`.
  - Minimal root task printing `BootInfo` summary (RAM untypeds, device untypeds, CNode size) to PL011 UART and powering off QEMU cleanly.
  - `tests/run_qemu_tests.py` headless runner.
- **Exit Criteria:**
  - Single-command build produces `fastix-image.elf` with zero warnings in Fastix code.
  - Headless QEMU boots seL4, prints verified RAM/device inventory, and exits cleanly with status 0.

### Phase 1: `libos`, Untyped/Cap Allocator, IPC Protocol & Multi-Task Bootstrap
- **Objectives:** Build the core freestanding OS library and prove isolated multi-process capability/IPC communication with zero leaks.
- **Deliverables:**
  - `libs/libos`: buddy untyped allocator (kernel objects are carved from untyped blocks and freed by revoking the block, so the kernel re-zeroes memory and every derived cap is removed), CSpace slot allocator, intrusive list, simple `memcpy`/`memset`/`memmove`/`memcmp`, debug print.
  - `arch/aarch64/`: page/page-table mapping wrappers, counter helpers (`KernelArmExportVCNTUser=ON`), register context helpers.
  - `procd`: kernel object allocator on top of the buddy allocator, minimal static ELF loader (embedded test child), and a bootstrap that spawns a separate unprivileged test child process with fixed CSpace slots, badged IPC endpoint, and badged fault endpoint.
  - *Moved to the phase that first uses them (Section 2.1 rule 6, no code without a real user):* slab allocator + interval tree + frame metadata array → Phase 2; SPSC ring → Phase 3; hash table + radix tree → Phase 4; rank-checked locks → first multi-threaded server (Phase 9); warm object pools + SIMD `memcpy`/`memset` → when a benchmark shows the need.
- **Exit Criteria:**
  - Host unit tests for the buddy untyped allocator and the slot allocator pass under ASan+UBSan across 100+ randomized seeds, checking exact model equivalence and full coalescing (0 leaked blocks).
  - QEMU test: child calls `procd` IPC 100,000 times, triggers controlled fault caught by `procd`, child is destroyed, and **0 bytes / 0 capabilities leaked** across 100 create/destroy cycles.

### Phase 2: Virtual Memory (VMA, Demand Paging, COW), ELF64 Loader & VDSO
- **Objectives:** Full Unix virtual memory semantics, static ELF64 loading, and zero-IPC VDSO.
- **Deliverables:**
  - `libs/libos` slab allocator, interval tree, and physical frame metadata array (moved from Phase 1), each with host tests under ASan+UBSan across 100+ randomized seeds.
  - `procd` VMA interval tree (`mmap`, `munmap` with splitting, `mprotect` with splitting/merging, `brk`).
  - Demand-zero paging, stack auto-growth + guard-page `SIGSEGV`, Copy-on-Write (COW) refcounted frames (including single-ref fast upgrade).
  - Static & PIE ELF64 loader with System V initial stack (`argc`, `argv`, `envp`, `auxv`).
  - Global VDSO page + per-process read-only info page + `seqlock` clock reading.
  - *Implementation notes (2026-11-19):* minimal `fork`/`exit`/`wait4` were built here because copy-on-write and fault tests need them (Phase 3 completes them). procd switched to a two-level CSpace (65,536 slots), and user frames come from a pool of 2 MB chunks, because one block + 3 slots per page could not back an 8 MB stack (measured: SIGBUS at 3.6 MB). `MAP_SHARED` (anonymous → Phase 3 with full `fork` semantics; file-backed → Phase 4) and a VDSO code page / `AT_SYSINFO_EHDR` (→ Phase 3 with the `sigreturn` trampoline) are deferred.
- **Exit Criteria:**
  - QEMU behavioral tests verify: NULL deref → `SIGSEGV`; stack overflow into guard page → `SIGSEGV`; `mprotect(PROT_READ)` write → `SIGSEGV`; partial `munmap` splits VMA cleanly; COW write modifies child without altering parent; VDSO `getpid`/`clock_gettime` returns monotonic accurate values with 0 IPCs; 500× `mmap+touch+munmap` cycles leak 0 frames.

### Phase 3: POSIX Process Lifecycle, Signals, Threads, Futex & Pipes
- **Objectives:** Complete POSIX execution & concurrency model inside `procd`.
- **Deliverables:**
  - `fork`, `vfork`, `posix_spawn_fast`, `execve` (ELF64 + `#!`), `exit`, `exit_group`, `waitpid`/`waitid` (incl. `WNOHANG`, `WUNTRACED`, `WCONTINUED`), process groups & sessions, credentials.
  - Full signal delivery with `sigaction`, `sigprocmask`, `sigaltstack`, VDSO `sigreturn` trampoline, `EINTR` vs `SA_RESTART`, realtime queued signals, `kill`/`tgkill`/`killpg`.
  - Thread creation (`TLS` via `tpidr_el0`), futex (`WAIT`/`WAKE`/`REQUEUE`/robust lists), high-res timers (`nanosleep`, `alarm`, `setitimer`, `timer_create`, `timerfd`), `eventfd`.
  - Zero-syscall streaming SPSC shared-memory `pipe`/`pipe2` with `PIPE_BUF` atomicity and `SIGPIPE`/`EPIPE` (`libs/libos` SPSC ring moved from Phase 1, with host tests under ASan+UBSan across 100+ randomized seeds).
- **Exit Criteria:**
  - QEMU behavioral tests: 1,000× `fork+exit+waitpid` with 0 byte/cap leaks; nested signal handlers + `sigaltstack` + FPU register preservation across signals; multithreaded futex contention test (8 threads, 100,000 ops); pipe throughput & atomic 4 KB writer stress test; `SIGCHLD` + zombie reaping + orphan reparenting to PID 1.

### Phase 4: `devd`, Isolated Drivers, `vfsd`, Unified Page Cache & Persistent Storage
- **Objectives:** Real hardware/VirtIO drivers, full VFS, unified page cache, and crash-clean persistent `ext2` filesystem.
- **Deliverables:**
  - `libs/libos` hash table and radix tree (moved from Phase 1), with host tests under ASan+UBSan across 100+ randomized seeds.
  - `libs/libdriver` (MMIO, DMA pool, IRQ handler wrapper, DTB parser).
  - `devd` + isolated drivers: `uart_pl011`, `timer` + `rtc_pl031`, `virtio_rng`, `virtio_blk` (GPT/MBR partitions).
  - `vfsd`: mount table, dentry/inode cache, full POSIX path walk (`*at`, symlinks, `ELOOP`, `..` across mounts), FD table + `dup`/`fcntl` locks/`flock`, unified page cache integrated with `procd` `mmap`.
  - Filesystems: `cpiofs`, `tmpfs`, `devfs`, `procfs`, and full read/write **`ext2`**.
- **Exit Criteria:**
  - Host unit tests + randomized fuzz tests on `ext2` and path-walker (100+ seeds).
  - QEMU tests: write files to `ext2` disk image, `sync`, reboot QEMU, verify files & metadata persist byte-for-byte; verify `mmap(MAP_SHARED)` writes are immediately visible to `read()` and vice versa; verify unlinked-but-open file reads/writes work and free blocks on final `close()`; verify driver isolation by faulting a test driver and observing `devd` clean recovery.

### Phase 5: Full `musl libc` Port (Static) & `fastix-cc` Sysroot
- **Objectives:** Any portable C17/POSIX program compiles with `fastix-cc` and runs on Fastix.
- **Deliverables:**
  - `libs/libfastix_sys` (complete syscall dispatcher, VDSO fastpath, pipe ring fastpath, FD-owner routing cache).
  - `libc/musl_port` wired cleanly to Fastix ABI; `sysroot` generation (`libc.a`, `crt*.o`, POSIX headers).
  - `fastix-cc` compiler wrapper.
- **Exit Criteria:**
  - Build and run **`libc-test`** (musl's official test suite) inside QEMU: >= 98% pass on all implemented subsystems (math, stdio, stdlib, string, pthread, signal, time, unistd, regex, malloc, mmap, functional tests).
  - Compile and run unmodified upstream **Lua** and **SQLite3** test suites on Fastix with 0 modifications to upstream source!
  - Static stripped `hello.c` binary <= 24 KB.

### Phase 6: `ttyd` (Termios + PTYs), PID 1 `init`, `dash` Shell & `toybox` Userland
- **Objectives:** Fastix becomes a complete, fast, interactive, daily-usable POSIX Unix OS!
- **Deliverables:**
  - `ttyd` with full `termios`, line editing, `ISIG`/`SIGTSTP`/`SIGTTIN`/`SIGTTOU`/`SIGWINCH`, `/dev/ptmx` + `/dev/pts/N`.
  - PID 1 `/sbin/init`, `/etc/rc`, `/etc/{passwd,group,fstab,profile}`.
  - **`dash`** as `/bin/sh`, **`toybox`** (allowlisted `fastix.config`), and **`onetrue-awk`**, all built static with `fastix-cc` from pinned upstreams with minimal patches (Section 8).
  - `userland/INVENTORY.md` mapping every Section 8.4 command to its source, status, and test.
  - `rootfs/mkrootfs.py` deterministic boot image builder.
- **Exit Criteria:**
  - Cold boot in QEMU directly to interactive `/bin/sh` prompt in <= 1.5 s (TCG) / <= 200 ms (HVF), RAM usage <= 16 MB, userland binaries within Section 1.4 budgets.
  - Automated PTY test script exercises interactive line editing, Ctrl-C (`SIGINT`), Ctrl-Z (`SIGTSTP`), `fg`/`bg`, pipes, and redirections under `dash`.
  - Differential userland tests (Section 8.4): toybox test suite, shell conformance corpus, and awk `REGRESS` produce output identical to host builds of the same pins.
  - `pjdfstest` core VFS compliance suite passes on both `tmpfs` and `ext2`.

### Phase 7: `netd`, `AF_UNIX` (`SCM_RIGHTS`), TCP/IPv4/IPv6 & Unified `poll`/`select`/`epoll`
- **Objectives:** Full POSIX networking, local IPC, and unified event multiplexing across all FD types.
- **Deliverables:**
  - Isolated `virtio_net` driver with zero-copy RX/TX rings.
  - `netd`: `AF_UNIX` (`STREAM`/`DGRAM`/`SEQPACKET`, `SCM_RIGHTS` FD passing, `SCM_CREDENTIALS`), loopback `lo`, Ethernet/ARP/NDP, IPv4/IPv6, ICMP, UDP, full RFC-compliant TCP.
  - Unified Event Broker supporting `poll`/`ppoll`, `select`/`pselect`, `epoll_create1`/`epoll_ctl`/`epoll_pwait` simultaneously over sockets, pipes, PTYs, `eventfd`, `timerfd`, and `signalfd`.
  - Userland network tools: `ip`, `ping`, `nc`, `dhcpc`, `wget`, DNS resolver (`/etc/resolv.conf` via musl).
- **Exit Criteria:**
  - QEMU networking tests (with QEMU user/tap networking): DHCP lease acquisition, DNS lookup, `ping`, HTTP download via `wget`, TCP echo/throughput test, IPv6 loopback test.
  - `AF_UNIX` `SCM_RIGHTS` test passes an open file FD from Process A to Process B and verifies Process B reads from the exact shared offset.
  - Mixed `epoll` stress test simultaneously multiplexing 256 sockets + 64 pipes + 16 PTYs + timers without lost wakeups or deadlocks.

### Phase 8: Dynamic Linker (`ld.so`), `ext4` Journaling & Full POSIX Conformance Closure
- **Objectives:** Shared libraries, crash-resilient filesystem, and closing every remaining POSIX.1-2017/2024 edge case.
- **Deliverables:**
  - `procd` dynamic ELF loader (`PT_INTERP` + full `auxv`), musl `libc.so` / `ld-musl-aarch64-fastix.so.1`, `dlopen`/`dlsym`/`dlclose`.
  - `ext4` (extents + `jbd2` journal replay on mount after simulated power-cut) + `fat32` + optional `virtio-fs`.
  - Remaining POSIX APIs: POSIX `mq_*`, SysV `shm`/`sem`, `aio_*`, `pwritev`/`preadv`, `sendfile`, `copy_file_range`, `fallocate`, `F_OFD_SETLK`, full `OPTS` gaps.
- **Exit Criteria:**
  - Dynamic binaries and `dlopen` plugin tests pass; shared `libc.so` pages are physically shared across 10 concurrent processes (verified via `procd` memory stats).
  - Power-cut crash test: kill QEMU mid-heavy `ext4` write workload 20 times, remount, verify journal replay restores a 100% consistent filesystem (`e2fsck -fn` on host reports 0 errors!).
  - `libc-test`, `OPTS` core, and `pjdfstest` >= 99% pass rate.

### Phase 9: SMP Multi-Core, `x86_64` Port, Real-World Ports & Self-Hosting
- **Objectives:** True multi-core scaling, multi-architecture portability, and native self-hosting.
- **Deliverables:**
  - Enable `KernelMaxNumNodes=4+` in seL4; per-CPU scheduler load balancing in `procd`, fine-grained locking / sharded server endpoints across cores, rank-checked locks in `libs/libos` (moved from Phase 1).
  - Complete `arch/x86_64/` (`q35`, APIC/HPET/COM1/VirtIO-PCI, `FS_BASE` TLS, x86_64 signal trampoline with 128-byte red zone) + `musl` x86_64 port.
  - Real-world third-party C ports: **Dropbear SSH** (remote SSH login into Fastix over TCP + PTY!), **bmake/gmake**, and **TinyCC (`tcc`)**. (Native Clang/GCC is a post-v1.0 stretch goal; see Section 8.6.)
- **Exit Criteria:**
  - SMP torture test (4 vCPUs): concurrent parallel builds, multithreaded futex/COW/VFS/socket stress runs 100+ iterations with zero deadlocks, race conditions, or memory corruption.
  - Both `aarch64` and `x86_64` pass the full headless QEMU test suite.
  - **Self-Hosting Proof:** Boot Fastix, run `tcc` + `make` *inside* Fastix to rebuild `dash`, `onetrue-awk`, and Fastix `init` from source, then run the new binaries through their Section 8.4 tests (rebuilding `toybox` with `tcc` is a stretch goal, since toybox may rely on compiler features tcc lacks).

### Phase 10: Deep Performance Tuning, Footprint Minimization & Security Audit
- **Objectives:** Hit every Section 1.4 performance and footprint target, harden attack surfaces, and tag v1.0.
- **Deliverables:**
  - Full `bench/micro` and `bench/macro` profiling pass; optimize top bottlenecks (fastpath verification, superpages, cache-line layout, LTO, lock contention).
  - Security & hardening audit: syscall fuzzer (1000+ seeds), `copy_from_user`/`copy_to_user` bounds audit, W^X audit, capability least-privilege audit, uninitialized padding zeroing audit (no kernel/server stack leaks to userland), ASLR enablement.
  - Reproducible release artifacts + hardware bring-up guide (Raspberry Pi 4/5).
- **Exit Criteria:**
  - All Section 1.4 size and latency targets met (or deviations documented with measured evidence and ADRs).
  - 24-hour continuous fuzzing + stress soak test passes with 0 server crashes and 0 capability/memory leaks.

---

## 13. Risk Register & Architectural Mitigations

| Risk | Impact | Root Cause on seL4 | Concrete Mitigation |
| :--- | :--- | :--- | :--- |
| **1. Microkernel IPC Overhead Kills Performance** | High | Naive 1-IPC-per-syscall + multi-server hops makes Unix 10–50× slower than Linux. | VDSO zero-IPC calls, userspace futexes, SPSC shared-memory rings, direct client→server routing (`procd`/`vfsd`/`netd`/`ttyd`), seL4 `ReplyRecv` fastpath, zero-copy page-cache frame sharing. |
| **2. `fork()` Latency Too Slow on seL4** | High | Duplicating page tables and retyping kernel objects via individual seL4 syscalls is costly. | Warm pre-retyped object pools (TCB/PT/CNode/Frame), sparse VSpace traversal (skip empty subtrees), COW refcounting, and first-class `posix_spawn`/`vfork` in libc and in userland where measured to matter. |
| **3. Cross-Server Deadlocks (`procd` ↔ `vfsd` ↔ `netd`)** | Critical | Synchronous circular `seL4_Call` chains freeze the whole system. | Strict acyclic IPC rank hierarchy (Section 14.5); asynchronous `seL4_Notification` for upward events; event-driven non-blocking server loops using saved `seL4_Reply` objects. |
| **4. Capability & Untyped Memory Leaks** | High | Forgetting to revoke/recycle one PageTable, Reply, or Frame cap on process exit slowly exhausts RAM. | Ownership list per `struct proc`; centralized object pool accounting; automated 1000-cycle leak gate in CI asserting exact 0-byte delta. |
| **5. Blocking Syscall Signal Interruption (`EINTR`)** | Medium | Signals arrive at `procd` while a thread is blocked inside `vfsd`, `ttyd`, or `netd`. | `procd` maintains `blocked_on_server` state per thread; when an unblocked signal arrives, `procd` posts an async `CANCEL_WAITER(tid)` notification to that server, which immediately replies `-EINTR` to the saved Reply cap. |
| **6. macOS Host Cross-Build Friction for seL4** | Medium | seL4 upstream scripts assume GNU tools and ELF cross-linkers. | Pin exact Clang + `ld.lld` + Python `.venv` setup in Phase 0; isolate all host-tool invocations behind portable CMake/Python wrappers; keep optional Linux container recipe as backup. |
| **7. Subtle POSIX Corner Cases Breaking Real C Programs** | Medium | Real software (Git, SQLite, SSH, Make) relies on obscure `fcntl` locks, `SCM_RIGHTS`, PTY job control, `mmap`+`read` coherency. | Run real upstream test suites early (`libc-test` in Phase 5, Lua/SQLite in Phase 5, `pjdfstest` in Phase 6, Dropbear/Make in Phase 9) — never rely solely on toy self-written tests. |
| **8. `procd` Becoming a Bloated Monolith** | Medium | Putting proc + VM + signals + pipes in `procd` risks growing it too complex. | Keep `procd` strictly scoped (no filesystems, no networking, no hardware drivers, no terminal logic); enforce internal module boundaries (`proc/`, `vm/`, `sig/`, `ipc/`) and strict size budget (<= 192 KB). |

---

## 14. Single-Source-of-Truth Reference Tables (Frozen Baseline Constants)

When Phase 0 initializes `include/fastix/`, these exact values MUST be encoded once and never duplicated elsewhere:

### 14.1 Core System Limits (`include/fastix/config.h`)
- `FASTIX_PAGE_SIZE = 4096` (`FASTIX_PAGE_SHIFT = 12`)
- `FASTIX_SUPERPAGE_SIZE = 2097152` (`2 MB`)
- `FASTIX_MAX_PROCS = 4096` (PID range `1..32767`, expandable)
- `FASTIX_MAX_THREADS_PER_PROC = 1024`
- `FASTIX_DEFAULT_NOFILE = 1024` (`FASTIX_MAX_NOFILE = 65536`)
- `FASTIX_PATH_MAX = 4096`, `FASTIX_NAME_MAX = 255`, `FASTIX_SYMLOOP_MAX = 40`
- `FASTIX_ARG_MAX = 131072` (128 KB)
- `FASTIX_PIPE_BUF = 4096` (POSIX atomic write guarantee), `FASTIX_PIPE_RING_SIZE = 65536` (64 KB)
- `FASTIX_SHM_IO_WINDOW = 65536` (64 KB per-server inline shared I/O buffer)
- `FASTIX_DEFAULT_STACK_SIZE = 8388608` (8 MB main stack), `FASTIX_THREAD_STACK_DEFAULT = 131072` (128 KB pthread default, matching musl efficiency)
- `FASTIX_SIGQUEUE_MAX = 64`
- `FASTIX_SCM_MAX_FD = 253` (max FDs per `SCM_RIGHTS` message), `FASTIX_UNIX_INFLIGHT_MAX = 4096` (max in-flight passed FDs per user)
- `FASTIX_ESTALEFD = 4095` (internal-only stale-FD-mirror code from Section 6.4, defined in `errno.h` above every real errno, NEVER returned to applications)
- `FASTIX_ARCH_NAME` (`"aarch64"` / `"x86_64"`, set ONLY by `arch/<arch>/`)
- `FASTIX_LDSO_PATH = "/lib/ld-musl-" FASTIX_ARCH_NAME "-fastix.so.1"` (e.g. `/lib/ld-musl-aarch64-fastix.so.1`; the ONLY place the dynamic linker name is spelled out — the ELF loader, musl build, and `fastix-cc` all derive it from here)

### 14.2 Badge Encoding (`include/fastix/cspace.h`)
- 64-bit seL4 Endpoint Badge layout for `procd`:
  - Bits `63..60`: `BADGE_TYPE` (`0x1` = User Syscall, `0x2` = User Fault, `0x3` = Core Server Control, `0x4` = Async Notification)
  - Bits `59..32`: `pid` (28 bits)
  - Bits `31..16`: `tid_slot` (16 bits)
  - Bits `15..0`: `generation` (16 bits — prevents stale-badge ABA reuse!)
- For `vfsd`/`netd`/`ttyd`: `badge = client_slot_id` (direct O(1) table index, revoked on process exit).

### 14.3 IPC Error & Return Convention (`include/fastix/abi.h`)
- Return `MR0 >= 0`: success value.
- Return `MR0` in `[-4095, -1]`: negated POSIX `errno` (`-ENOENT`, `-EAGAIN`, `-EINTR`, `-EFAULT`, etc.).
- Every `errno` constant in `include/fastix/errno.h` matches the canonical AArch64/Linux/musl generic `errno-base.h` + `errno.h` numbering so musl needs zero errno translation tables!

### 14.4 Server Memory & Binary Budgets (`include/fastix/config.h`)
Core OS server budgets sum cleanly to the Section 1.4 total budget of `<= 512 KB`:
- `procd`: <= 192 KB `.text+.rodata`
- `vfsd`: <= 160 KB `.text+.rodata` (incl. `tmpfs`, `devfs`, `procfs`, `ext2`/`ext4`)
- `netd`: <= 96 KB `.text+.rodata`
- `ttyd`: <= 32 KB `.text+.rodata`
- `devd`: <= 32 KB `.text+.rodata`
- *(Total core servers: `192 + 160 + 96 + 32 + 32 = 512 KB`)*
- Each isolated driver: <= 32 KB `.text+.rodata`

### 14.5 Strict Acyclic Cross-Server IPC & Lock Rank Hierarchy (`include/fastix/lock_rank.h`)
Synchronous `seL4_Call` is ONLY permitted strictly downward (from higher rank number to lower rank number). Upward communication MUST use non-blocking `seL4_Signal` notifications + async work rings or client-initiated retries:

```text
Rank 50: Userland Processes (clients)
   │  (may seL4_Call Ranks 40, 30, 20, 10)
   ▼
Rank 40: netd / ttyd  (leaf service servers)
   │  (may seL4_Call Ranks 30, 20, 10 — e.g. FD install/lookup, AF_UNIX path binding, SCM_RIGHTS pinning in vfsd; never each other)
   ▼
Rank 30: vfsd  (filesystem & page cache)
   │  (may seL4_Call Ranks 20, 10)
   ▼
Rank 20: devd & Isolated Drivers (uart, timer, virtio_blk, virtio_net, virtio_rng)
   │  (may seL4_Call Rank 10 for memory/cap requests ONLY at init/reclaim; async Notify upward)
   ▼
Rank 10: procd  (root executive — NEVER makes a blocking seL4_Call to ANY other server!)
   │  (uses ONLY async work rings + seL4_Signal for VFS fault/fork/exec/exit sync and waiter cancellation)
   ▼
Rank 00: seL4 Microkernel
```

*(Note on `procd` ↔ `vfsd` coordination: Whenever `procd` needs `vfsd` — for a file-backed `mmap` page fault, `fork()` FD duplication, `execve()` `O_CLOEXEC` cleanup, or process exit FD teardown — `procd` NEVER blocks its main loop calling `vfsd`. Instead, `procd` places the waiting thread in a pending state, enqueues an asynchronous work item into the `procd -> vfsd` lock-free ring + `seL4_Signal`, and immediately returns to its main loop. When `vfsd` finishes, `vfsd` (Rank 30) makes a normal downward `seL4_Call` to `procd` (Rank 10) delivering the frame capability or completion acknowledgment, and `procd` resumes/replies to the thread. **Result: Strictly acyclic, mathematically deadlock-free.**)*

---

## 15. Immediate Next Step (Ready for Phase 0)
No source code has been written yet. When the user approves this plan and gives the go-ahead to begin implementation, execute **Phase 0** first:
0. Create `PROGRESS.md` (Section 2, rule 10) with Phase 0 as the current phase and its exit criteria as the checklist.
1. Verify/install missing host cross-linkers (`lld` / `aarch64-elf` toolchain) and Python seL4 build venv.
2. Initialize the repository layout, pin upstream `seL4` and `seL4_tools`, and create the single-source-of-truth headers in `include/fastix/`.
3. Build and boot unmodified seL4 (AArch64 MCS, `qemu-arm-virt`) to serial output inside `tests/run_qemu_tests.py` and verify clean automated pass.
