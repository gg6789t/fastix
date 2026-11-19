# procd

The seL4 root task and process executive (AGENTS.md 6.3 B).

**Current (Phase 2):** procd boots, switches to its own larger CSpace, starts PID 1 from an embedded
program image, and then serves syscalls and page faults in one event loop.

- **IPC operations:** `exit`/`exit_group`, `getpid`, `getppid`, `brk`, `mmap` (anonymous private only),
  `munmap`, `mprotect`, `fork`, `wait4` (pid -1 or > 0, `WNOHANG`), `clock_gettime`, `sched_yield`.
  With `FASTIX_TEST_HOOKS`: test-only labels from `fastix/abi.h` (ping, stats, last kill, spawn image).
- **Faults:** page faults are resolved (demand-zero, copy-on-write, sole-owner upgrade) or end the
  process with SIGSEGV (MAPERR/ACCERR) or SIGBUS (out of memory). User exceptions give SIGILL.
- **Modules and data structures:**
  - `cspace.c`: two-level CSpace (65,536 slots); the boot CNode stays at level-1 entry 0.
  - `kobj.c`: kernel objects, one buddy untyped block each (free = revoke).
  - `heap.c`: procd heap pages for slab caches, plus procd's own page tables.
  - `frame.c`: user frames, pooled in 2 MB chunks, reference-counted, zeroed before reuse.
  - `vmspace.c`: per-process VMAs and pages (two `rtree`s) and page tables. Copy-on-write rule:
    writable only if the VMA allows it and the frame is not shared.
  - `loader.c`: static ELF and static-PIE loading, plus the System V initial stack.
  - `proc.c`: process table (pid slot = pid % 4096), spawn, fork, exit/zombie, wait.
  - `reply.c`: pool of MCS Reply objects; a blocked `wait4` parks its Reply object.
  - `vdso.c`: the global clock page (seqlock) and per-process info pages.
  - `server.c`: the event loop and syscall dispatch. `images.c`: embedded program images.
- **Locks / rank:** single-threaded, so no locks; IPC rank 10 (AGENTS.md 14.5).
