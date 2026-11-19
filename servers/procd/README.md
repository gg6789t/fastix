# procd

The seL4 root task and process executive (AGENTS.md 6.3 B).

**Current (Phase 3):** procd boots, switches to its own two-level CSpace, starts PID 1 from an
embedded image, and serves process lifecycle, VM, signals, timers, threads, futex, pipes, and
eventfd in a single-threaded non-blocking event loop.

- **IPC operations:**
  - Process & thread: `exit` (thread), `exit_group`, `fork`, `vfork`, `posix_spawn_fast`, `execve`
    (ELF64 + `#!` depth <= 4), `wait4` (pid/-1/0/-pgid, `WNOHANG`/`WUNTRACED`/`WCONTINUED`),
    `thread_create`, `set_tid_address`, `set_tls`, `getpid`/`getppid`/`gettid`,
    `getpgid`/`setpgid`/`getsid`/`setsid`, `get/set{uid,euid,reuid,resuid,gid,egid,regid,resgid}`,
    `umask`, `sched_yield`.
  - Memory: `brk`, `mmap` (anonymous `MAP_PRIVATE` & `MAP_SHARED`), `munmap`, `mprotect`.
  - Signals: `rt_sigaction`, `rt_sigprocmask`, `rt_sigpending`, `rt_sigsuspend`, `rt_sigreturn`,
    `sigaltstack`, `kill`, `tkill`, `tgkill`, `rt_sigqueueinfo`.
  - Timers: `clock_gettime`, `nanosleep`, `clock_nanosleep`, `setitimer`/`getitimer` (`ITIMER_REAL`),
    `timer_create`/`settime`/`gettime`/`getoverrun`/`delete`.
  - Sync & local IPC: `futex` (`WAIT`/`WAKE`/`REQUEUE`/`CMP_REQUEUE`/`WAIT_BITSET`/`WAKE_BITSET`,
    private & shared), `set_robust_list`/`get_robust_list`, `pipe2`, `eventfd2`, plus temporary
    Phase 3 `read`/`write`/`close`/`dup`/`dup3`/`fcntl` on pipe/eventfd FDs (moves to `vfsd` in Phase 4).
  - With `FASTIX_TEST_HOOKS`: test labels in `fastix/abi.h` (ping, stats, vm stats, last kill, ipc count, spawn).
- **Faults:** VM faults resolve demand-zero, COW, sole-owner upgrade, and shared pages; unhandled
  faults deliver `SIGSEGV`/`SIGBUS`/`SIGILL` to a user handler if installed, else terminate the process.
- **Modules and data structures:**
  - `cspace.c`, `kobj.c`, `heap.c`, `frame.c`: two-level procd CSpace (65,536 slots), buddy untyped
    objects, slab heap, pooled refcounted 4 KB frames.
  - `vmspace.c`: refcounted `vmspace` (shared during `vfork`), VMA/page `rtree`s, lazy `anon_shm`
    backing for `MAP_SHARED`, safe `vms_read`/`vms_write_user`.
  - `loader.c`, `exec.c`, `images.c`: ELF64 + `#!` loader, System V stack/auxv (`AT_SYSINFO_EHDR`),
    `execve` and `posix_spawn_fast`.
  - `proc.c`: process table, `struct thread` list, lazy level-2 `THREAD_CNODE`, `fork`/`vfork`,
    `thread_create`/`exit`, `wait4`, pgid/sid, credentials, orphan reparenting.
  - `signal.c`, `vdso.c`: 64-signal queues (standard + RT FIFO), user-stack frame delivery through
    VDSO `__fastix_sigenter`/`__fastix_sigreturn` (saving/restoring all GP + 32×128-bit FPU regs),
    `sigaltstack`, `SIGSTOP`/`SIGCONT`, `EINTR` vs `SA_RESTART`.
  - `timer.c`: sleeps, `ITIMER_REAL`, POSIX interval timers, driven by a suspended-when-idle 1 ms
    MCS ticker signaling a notification bound to procd's main thread.
  - `futex.c`: 64-bucket private/shared futex hash waitqueues, timeouts, robust list exit walk.
  - `pipe.c`: 64 KB SPSC-ring pipes (17 pooled frames each, `PIPE_BUF` atomicity), `eventfd2`, and
    the temporary 64-FD per-process table (Phase 3 only).
  - `reply.c`, `server.c`: MCS Reply pool and non-blocking dispatch loop.
- **Locks / rank:** single-threaded event loop (no locks); IPC rank 10 (AGENTS.md 14.5).
