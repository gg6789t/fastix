# procd

The seL4 root task and process executive (AGENTS.md 6.3 B).

**Current (Phase 4C):** procd boots, switches to its own two-level CSpace, spawns isolated `devd`
(handing over the runtime DTB, Device Untypeds, `IRQControl`, and a 4 MB RAM untyped), starts PID 1
from an embedded image, starts `vfsd` as a system process (PID 2), and serves process lifecycle, VM, signals, timers, threads, futex, pipes,
eventfd, and downward Rank 20 → Rank 10 server control calls in a single-threaded event loop.

- **IPC operations:**
  - Process & thread: `exit` (thread), `exit_group`, `fork`, `vfork`, `posix_spawn_fast`, `execve`
    (ELF64 + `#!` depth <= 4), `wait4` (pid/-1/0/-pgid, `WNOHANG`/`WUNTRACED`/`WCONTINUED`), `waitid` (`P_ALL`/`P_PID`/`P_PGID`,
    `WEXITED`/`WSTOPPED`/`WCONTINUED`/`WNOHANG`/`WNOWAIT`, SIGCHLD `siginfo`),
    `thread_create`, `set_tid_address`, `set_tls`, `getpid`/`getppid`/`gettid`,
    `getpgid`/`setpgid`/`getsid`/`setsid`, `get/set{uid,euid,reuid,resuid,gid,egid,regid,resgid}`,
    `umask`, `sched_yield`.
  - Memory: `brk`, `mmap` (anonymous `MAP_PRIVATE` & `MAP_SHARED`), `munmap`, `mprotect`.
  - Signals: `rt_sigaction`, `rt_sigprocmask`, `rt_sigpending`, `rt_sigsuspend`, `rt_sigtimedwait`, `rt_sigreturn`,
    `sigaltstack`, `kill`, `tkill`, `tgkill`, `rt_sigqueueinfo`.
  - Timers: `clock_gettime`, `nanosleep`, `clock_nanosleep`, `setitimer`/`getitimer` (`ITIMER_REAL`),
    `timer_create`/`settime`/`gettime`/`getoverrun`/`delete`.
  - Sync & local IPC: `futex` (`WAIT`/`WAKE`/`REQUEUE`/`CMP_REQUEUE`/`WAIT_BITSET`/`WAKE_BITSET`,
    private & shared), `set_robust_list`/`get_robust_list`, `pipe2`, `eventfd2`, and `read`/`write`
    on pipe/eventfd FDs (found through vfsd's exported FD table; other FDs answer `-ESTALEFD`).
  - Server control (`FASTIX_BADGE_SERVER`, Rank 20 → Rank 10): `FASTIX_SRV_SET_REALTIME_NS`
    (updates VDSO `realtime_offset_ns` under seqlock), `FASTIX_SRV_REGISTER_DRV` (stores driver
    `service_ep` caps registered by `devd`).
  - From `vfsd` (`FASTIX_SRVID_VFSD`, rank 30 → 10): `FASTIX_SRV_VFS_WORK` (pull the next work
    item: NEW, EXEC, EXIT, INSTALL), `FASTIX_SRV_OBJ_RELEASE`, `FASTIX_SRV_OBJ_SETFL`.
  - With `FASTIX_TEST_HOOKS`: test labels in `fastix/abi.h` (ping, stats, vm stats, last kill, ipc
    count, spawn, get driver endpoint).
- **Faults:** VM faults resolve demand-zero, COW, sole-owner upgrade, and shared pages; unhandled
  faults deliver `SIGSEGV`/`SIGBUS`/`SIGILL` to a user handler if installed, else terminate the process.
- **Modules and data structures:**
  - `cspace.c`, `kobj.c` (on the shared `libs/libos/alloc/kalloc.c`), `heap.c`, `frame.c`: two-level
    procd CSpace (65,536 slots), buddy untyped objects, slab heap, pooled refcounted 4 KB frames.
  - `vmspace.c` (+ `vm_user.c`: checked reads/writes of user memory, `vm_internal.h`): refcounted `vmspace` (shared during `vfork`), VMA/page `rtree`s, lazy `anon_shm`
    backing for `MAP_SHARED`, safe `vms_read`/`vms_write_user`.
  - `loader.c`, `exec.c`, `images.c`: ELF64 + `#!` loader, System V stack/auxv (`AT_SYSINFO_EHDR`),
    `execve` and `posix_spawn_fast`.
  - `proc.c` (+ `thread.c`: threads; `proc_ids.c`: groups, sessions, credentials; `proc_internal.h`): process table, `struct thread` list, lazy level-2 `THREAD_CNODE`, `fork`/`vfork`,
    `thread_create`/`exit`, `wait4`, pgid/sid, credentials, orphan reparenting.
  - `signal.c` (+ `sig_sys.c`: signal syscalls; `sig_internal.h`), `vdso.c`: 64-signal queues (standard + RT FIFO), user-stack frame delivery through
    VDSO `__fastix_sigenter`/`__fastix_sigreturn` (saving/restoring all GP + 32×128-bit FPU regs),
    `sigaltstack`, `SIGSTOP`/`SIGCONT`, `EINTR` vs `SA_RESTART`.
  - `devboot.c`: parses `SEL4_BOOTINFO_HEADER_FDT` from `seL4_BootInfo`, spawns `devd`, moves
    Device Untypeds + `IRQControl` to `devd`, and handles `FASTIX_BADGE_SERVER` registration/RTC ops.
  - `timer.c` (+ `ticker.c`, kept on `qemu-arm-virt` per `docs/adr/0002-timer.md`): sleeps, `ITIMER_REAL`, POSIX interval timers, driven by a suspended-when-idle 1 ms
    MCS ticker signaling a notification bound to procd's main thread.
  - `futex.c`: 64-bucket private/shared futex hash waitqueues, timeouts, robust list exit walk.
  - `pipe.c`: 64 KB SPSC-ring pipes (17 pooled frames each, `PIPE_BUF` atomicity) and `eventfd2`;
    one numbered object per open file description, kept alive while a thread waits on it.
  - `vfslink.c`: the vfsd link (docs/adr/0003): starts vfsd, one client record per process (I/O
    window, procinfo and FD-export page mapped into vfsd), the work queue, and holding a new
    program's first thread until vfsd has set up its FD table.
  - `reply.c`, `server.c` (+ `server_vfs.c`: calls that wait for vfsd; `server_internal.h`): MCS
    Reply pool and non-blocking dispatch loop.
- **Locks / rank:** single-threaded event loop (no locks); IPC rank 10 (AGENTS.md 14.5).
