/*
 * fastix/abi.h - native syscall numbers and the IPC message convention
 * shared by libc and all servers (AGENTS.md 5.5, 7.3, 14.3).
 */
#ifndef FASTIX_ABI_H
#define FASTIX_ABI_H

#include <stdint.h>

/* IPC message layout. The seL4 message label carries the op code. */
#define FASTIX_IPC_LABEL_BITS        12  /* op codes 0..4095 */
#define FASTIX_IPC_FASTPATH_MRS      4   /* stay within seL4 fastpath limit */
#define FASTIX_IPC_MAX_MRS           16
#define FASTIX_IPC_INLINE_PATH_MAX   64  /* short paths may travel in MRs */
#define FASTIX_IPC_INLINE_DATA_MAX   48  /* small reads/writes may travel in MRs */

/* Labels at or above this value are reserved for test-only protocols. */
#define FASTIX_IPC_LABEL_TEST_BASE   0xF00

/* Reply registers. MR0 >= 0 is success, MR0 in [-4095, -1] is -errno. */
#define FASTIX_REPLY_MR_RET          0
#define FASTIX_REPLY_MR_RET2         1   /* second result (pipe fds, 64-bit offset, ...) */
#define FASTIX_REPLY_MR_SIGPENDING   2   /* procd replies: pending signal hint */

/* Auxiliary vector entries on the initial stack (values match musl <elf.h>). */
#define FASTIX_AT_NULL          0
#define FASTIX_AT_PHDR          3
#define FASTIX_AT_PHENT         4
#define FASTIX_AT_PHNUM         5
#define FASTIX_AT_PAGESZ        6
#define FASTIX_AT_BASE          7
#define FASTIX_AT_FLAGS         8
#define FASTIX_AT_ENTRY         9
#define FASTIX_AT_UID           11
#define FASTIX_AT_EUID          12
#define FASTIX_AT_GID           13
#define FASTIX_AT_EGID          14
#define FASTIX_AT_CLKTCK        17
#define FASTIX_AT_SECURE        23
#define FASTIX_AT_RANDOM        25
#define FASTIX_AT_EXECFN        31
#define FASTIX_AT_SYSINFO_EHDR  33
#define FASTIX_CLK_TCK          100

/* clock ids (match musl <time.h>) */
#define FASTIX_CLOCK_REALTIME   0
#define FASTIX_CLOCK_MONOTONIC  1
#define FASTIX_CLOCK_BOOTTIME   7

/* wait4 options (match musl <sys/wait.h>) */
#define FASTIX_WNOHANG          1
#define FASTIX_WUNTRACED        2
#define FASTIX_WCONTINUED       8

/* #! script interpreter recursion limit */
#define FASTIX_SHEBANG_MAX_DEPTH 4

/* posix_spawn_fast flags & argument struct (AGENTS.md 6.3 B) */
#define FASTIX_SPAWN_SETPGROUP   0x02u
#define FASTIX_SPAWN_SETSIGDEF   0x04u
#define FASTIX_SPAWN_SETSIGMASK  0x08u

struct fastix_spawn_args {
    uint64_t path;       /* user pointer to NUL-terminated path */
    uint64_t argv;       /* user pointer to NULL-terminated argv[] */
    uint64_t envp;       /* user pointer to NULL-terminated envp[] */
    uint32_t flags;      /* FASTIX_SPAWN_* */
    uint32_t pgroup;     /* target pgid if FASTIX_SPAWN_SETPGROUP (0 = child pid) */
    uint64_t sigmask;    /* signal mask if FASTIX_SPAWN_SETSIGMASK */
    uint64_t sigdefault; /* signals reset to SIG_DFL if FASTIX_SPAWN_SETSIGDEF */
};

/* Test-only procd operations (debug/test builds, FASTIX_TEST_HOOKS). */
#define FASTIX_TEST_PING        (FASTIX_IPC_LABEL_TEST_BASE + 0) /* MR0 n -> n + 1 */
#define FASTIX_TEST_STATS       (FASTIX_IPC_LABEL_TEST_BASE + 1) /* -> MR1..MR5 global counters */
#define FASTIX_TEST_VM_STATS    (FASTIX_IPC_LABEL_TEST_BASE + 2) /* -> MR1 vmas, MR2 pages of caller */
#define FASTIX_TEST_LAST_KILL   (FASTIX_IPC_LABEL_TEST_BASE + 3) /* MR0 pid -> MR1 sig, MR2 code, MR3 addr */
#define FASTIX_TEST_IPC_COUNT   (FASTIX_IPC_LABEL_TEST_BASE + 4) /* -> MR1 calls made by caller so far */
#define FASTIX_TEST_SPAWN       (FASTIX_IPC_LABEL_TEST_BASE + 5) /* MR0 image index -> new child pid */
#define FASTIX_TEST_GET_DRV_EP  (FASTIX_IPC_LABEL_TEST_BASE + 6) /* MR0 drv_kind -> installs cap in FASTIX_CAP_DRV_FIRST + kind */

/* Syscall groups. Each group owns the numbers from its base up to the next base. */
#define FASTIX_SYSGRP_PROC    0x000
#define FASTIX_SYSGRP_MEM     0x040
#define FASTIX_SYSGRP_SIG     0x050
#define FASTIX_SYSGRP_TIME    0x070
#define FASTIX_SYSGRP_SYNC    0x090
#define FASTIX_SYSGRP_FS      0x0B0
#define FASTIX_SYSGRP_NET     0x100
#define FASTIX_SYSGRP_SYS     0x130
#define FASTIX_SYSGRP_END     0x150

enum fastix_sysno {
    /* Process & thread (procd; * = served from VDSO/procinfo page) */
    FASTIX_SYS_exit             = FASTIX_SYSGRP_PROC + 0,
    FASTIX_SYS_exit_group       = FASTIX_SYSGRP_PROC + 1,
    FASTIX_SYS_fork             = FASTIX_SYSGRP_PROC + 2,
    FASTIX_SYS_vfork            = FASTIX_SYSGRP_PROC + 3,
    FASTIX_SYS_posix_spawn_fast = FASTIX_SYSGRP_PROC + 4,
    FASTIX_SYS_execve           = FASTIX_SYSGRP_PROC + 5,
    FASTIX_SYS_execveat         = FASTIX_SYSGRP_PROC + 6,
    FASTIX_SYS_wait4            = FASTIX_SYSGRP_PROC + 7,
    FASTIX_SYS_waitid           = FASTIX_SYSGRP_PROC + 8,
    FASTIX_SYS_thread_create    = FASTIX_SYSGRP_PROC + 9,
    FASTIX_SYS_set_tid_address  = FASTIX_SYSGRP_PROC + 10,
    FASTIX_SYS_set_tls          = FASTIX_SYSGRP_PROC + 11,
    FASTIX_SYS_getpid           = FASTIX_SYSGRP_PROC + 12, /* * */
    FASTIX_SYS_getppid          = FASTIX_SYSGRP_PROC + 13, /* * */
    FASTIX_SYS_gettid           = FASTIX_SYSGRP_PROC + 14,
    FASTIX_SYS_getpgid          = FASTIX_SYSGRP_PROC + 15, /* * for getpgid(0) */
    FASTIX_SYS_setpgid          = FASTIX_SYSGRP_PROC + 16,
    FASTIX_SYS_getsid           = FASTIX_SYSGRP_PROC + 17,
    FASTIX_SYS_setsid           = FASTIX_SYSGRP_PROC + 18,
    FASTIX_SYS_getuid           = FASTIX_SYSGRP_PROC + 19, /* * */
    FASTIX_SYS_geteuid          = FASTIX_SYSGRP_PROC + 20, /* * */
    FASTIX_SYS_getgid           = FASTIX_SYSGRP_PROC + 21, /* * */
    FASTIX_SYS_getegid          = FASTIX_SYSGRP_PROC + 22, /* * */
    FASTIX_SYS_setuid           = FASTIX_SYSGRP_PROC + 23,
    FASTIX_SYS_seteuid          = FASTIX_SYSGRP_PROC + 24,
    FASTIX_SYS_setreuid         = FASTIX_SYSGRP_PROC + 25,
    FASTIX_SYS_setresuid        = FASTIX_SYSGRP_PROC + 26,
    FASTIX_SYS_getresuid        = FASTIX_SYSGRP_PROC + 27,
    FASTIX_SYS_setgid           = FASTIX_SYSGRP_PROC + 28,
    FASTIX_SYS_setegid          = FASTIX_SYSGRP_PROC + 29,
    FASTIX_SYS_setregid         = FASTIX_SYSGRP_PROC + 30,
    FASTIX_SYS_setresgid        = FASTIX_SYSGRP_PROC + 31,
    FASTIX_SYS_getresgid        = FASTIX_SYSGRP_PROC + 32,
    FASTIX_SYS_getgroups        = FASTIX_SYSGRP_PROC + 33,
    FASTIX_SYS_setgroups        = FASTIX_SYSGRP_PROC + 34,
    FASTIX_SYS_umask            = FASTIX_SYSGRP_PROC + 35,
    FASTIX_SYS_getrlimit        = FASTIX_SYSGRP_PROC + 36,
    FASTIX_SYS_setrlimit        = FASTIX_SYSGRP_PROC + 37,
    FASTIX_SYS_prlimit64        = FASTIX_SYSGRP_PROC + 38,
    FASTIX_SYS_getrusage        = FASTIX_SYSGRP_PROC + 39,
    FASTIX_SYS_times            = FASTIX_SYSGRP_PROC + 40,

    /* Memory (procd) */
    FASTIX_SYS_brk              = FASTIX_SYSGRP_MEM + 0,
    FASTIX_SYS_mmap             = FASTIX_SYSGRP_MEM + 1,
    FASTIX_SYS_munmap           = FASTIX_SYSGRP_MEM + 2,
    FASTIX_SYS_mprotect         = FASTIX_SYSGRP_MEM + 3,
    FASTIX_SYS_mremap           = FASTIX_SYSGRP_MEM + 4,
    FASTIX_SYS_msync            = FASTIX_SYSGRP_MEM + 5,
    FASTIX_SYS_madvise          = FASTIX_SYSGRP_MEM + 6,
    FASTIX_SYS_mincore          = FASTIX_SYSGRP_MEM + 7,
    FASTIX_SYS_mlock            = FASTIX_SYSGRP_MEM + 8,
    FASTIX_SYS_munlock          = FASTIX_SYSGRP_MEM + 9,

    /* Signals (procd) */
    FASTIX_SYS_rt_sigaction     = FASTIX_SYSGRP_SIG + 0,
    FASTIX_SYS_rt_sigprocmask   = FASTIX_SYSGRP_SIG + 1,
    FASTIX_SYS_rt_sigpending    = FASTIX_SYSGRP_SIG + 2,
    FASTIX_SYS_rt_sigsuspend    = FASTIX_SYSGRP_SIG + 3,
    FASTIX_SYS_rt_sigtimedwait  = FASTIX_SYSGRP_SIG + 4,
    FASTIX_SYS_rt_sigreturn     = FASTIX_SYSGRP_SIG + 5,
    FASTIX_SYS_sigaltstack      = FASTIX_SYSGRP_SIG + 6,
    FASTIX_SYS_kill             = FASTIX_SYSGRP_SIG + 7,
    FASTIX_SYS_tkill            = FASTIX_SYSGRP_SIG + 8,
    FASTIX_SYS_tgkill           = FASTIX_SYSGRP_SIG + 9,
    FASTIX_SYS_rt_sigqueueinfo  = FASTIX_SYSGRP_SIG + 10,
    FASTIX_SYS_signalfd4        = FASTIX_SYSGRP_SIG + 11,

    /* Time & scheduling (procd; clock_gettime is VDSO) */
    FASTIX_SYS_clock_gettime      = FASTIX_SYSGRP_TIME + 0,
    FASTIX_SYS_clock_settime      = FASTIX_SYSGRP_TIME + 1,
    FASTIX_SYS_clock_getres       = FASTIX_SYSGRP_TIME + 2,
    FASTIX_SYS_clock_nanosleep    = FASTIX_SYSGRP_TIME + 3,
    FASTIX_SYS_nanosleep          = FASTIX_SYSGRP_TIME + 4,
    FASTIX_SYS_setitimer          = FASTIX_SYSGRP_TIME + 5,
    FASTIX_SYS_getitimer          = FASTIX_SYSGRP_TIME + 6,
    FASTIX_SYS_timer_create       = FASTIX_SYSGRP_TIME + 7,
    FASTIX_SYS_timer_settime      = FASTIX_SYSGRP_TIME + 8,
    FASTIX_SYS_timer_gettime      = FASTIX_SYSGRP_TIME + 9,
    FASTIX_SYS_timer_getoverrun   = FASTIX_SYSGRP_TIME + 10,
    FASTIX_SYS_timer_delete       = FASTIX_SYSGRP_TIME + 11,
    FASTIX_SYS_timerfd_create     = FASTIX_SYSGRP_TIME + 12,
    FASTIX_SYS_timerfd_settime    = FASTIX_SYSGRP_TIME + 13,
    FASTIX_SYS_timerfd_gettime    = FASTIX_SYSGRP_TIME + 14,
    FASTIX_SYS_sched_yield        = FASTIX_SYSGRP_TIME + 15,
    FASTIX_SYS_sched_getparam     = FASTIX_SYSGRP_TIME + 16,
    FASTIX_SYS_sched_setparam     = FASTIX_SYSGRP_TIME + 17,
    FASTIX_SYS_sched_getscheduler = FASTIX_SYSGRP_TIME + 18,
    FASTIX_SYS_sched_setscheduler = FASTIX_SYSGRP_TIME + 19,
    FASTIX_SYS_sched_getaffinity  = FASTIX_SYSGRP_TIME + 20,
    FASTIX_SYS_sched_setaffinity  = FASTIX_SYSGRP_TIME + 21,
    FASTIX_SYS_getpriority        = FASTIX_SYSGRP_TIME + 22,
    FASTIX_SYS_setpriority        = FASTIX_SYSGRP_TIME + 23,

    /* Sync & local IPC (procd) */
    FASTIX_SYS_futex            = FASTIX_SYSGRP_SYNC + 0,
    FASTIX_SYS_set_robust_list  = FASTIX_SYSGRP_SYNC + 1,
    FASTIX_SYS_get_robust_list  = FASTIX_SYSGRP_SYNC + 2,
    FASTIX_SYS_pipe2            = FASTIX_SYSGRP_SYNC + 3,
    FASTIX_SYS_eventfd2         = FASTIX_SYSGRP_SYNC + 4,
    FASTIX_SYS_shm_open_fast    = FASTIX_SYSGRP_SYNC + 5,
    FASTIX_SYS_mq_open          = FASTIX_SYSGRP_SYNC + 6,
    FASTIX_SYS_mq_unlink        = FASTIX_SYSGRP_SYNC + 7,
    FASTIX_SYS_mq_timedsend     = FASTIX_SYSGRP_SYNC + 8,
    FASTIX_SYS_mq_timedreceive  = FASTIX_SYSGRP_SYNC + 9,
    FASTIX_SYS_mq_getsetattr    = FASTIX_SYSGRP_SYNC + 10,
    FASTIX_SYS_semget           = FASTIX_SYSGRP_SYNC + 11,
    FASTIX_SYS_semop            = FASTIX_SYSGRP_SYNC + 12,
    FASTIX_SYS_semctl           = FASTIX_SYSGRP_SYNC + 13,
    FASTIX_SYS_shmget           = FASTIX_SYSGRP_SYNC + 14,
    FASTIX_SYS_shmat            = FASTIX_SYSGRP_SYNC + 15,
    FASTIX_SYS_shmdt            = FASTIX_SYSGRP_SYNC + 16,
    FASTIX_SYS_shmctl           = FASTIX_SYSGRP_SYNC + 17,

    /* Filesystem & FD (vfsd, with direct routing to owner servers) */
    FASTIX_SYS_openat           = FASTIX_SYSGRP_FS + 0,
    FASTIX_SYS_close            = FASTIX_SYSGRP_FS + 1,
    FASTIX_SYS_read             = FASTIX_SYSGRP_FS + 2,
    FASTIX_SYS_write            = FASTIX_SYSGRP_FS + 3,
    FASTIX_SYS_pread64          = FASTIX_SYSGRP_FS + 4,
    FASTIX_SYS_pwrite64         = FASTIX_SYSGRP_FS + 5,
    FASTIX_SYS_readv            = FASTIX_SYSGRP_FS + 6,
    FASTIX_SYS_writev           = FASTIX_SYSGRP_FS + 7,
    FASTIX_SYS_preadv           = FASTIX_SYSGRP_FS + 8,
    FASTIX_SYS_pwritev          = FASTIX_SYSGRP_FS + 9,
    FASTIX_SYS_lseek            = FASTIX_SYSGRP_FS + 10,
    FASTIX_SYS_dup              = FASTIX_SYSGRP_FS + 11,
    FASTIX_SYS_dup3             = FASTIX_SYSGRP_FS + 12,
    FASTIX_SYS_fcntl            = FASTIX_SYSGRP_FS + 13,
    FASTIX_SYS_ioctl            = FASTIX_SYSGRP_FS + 14,
    FASTIX_SYS_flock            = FASTIX_SYSGRP_FS + 15,
    FASTIX_SYS_fsync            = FASTIX_SYSGRP_FS + 16,
    FASTIX_SYS_fdatasync        = FASTIX_SYSGRP_FS + 17,
    FASTIX_SYS_sync             = FASTIX_SYSGRP_FS + 18,
    FASTIX_SYS_syncfs           = FASTIX_SYSGRP_FS + 19,
    FASTIX_SYS_truncate         = FASTIX_SYSGRP_FS + 20,
    FASTIX_SYS_ftruncate        = FASTIX_SYSGRP_FS + 21,
    FASTIX_SYS_fallocate        = FASTIX_SYSGRP_FS + 22,
    FASTIX_SYS_fstat            = FASTIX_SYSGRP_FS + 23,
    FASTIX_SYS_newfstatat       = FASTIX_SYSGRP_FS + 24,
    FASTIX_SYS_statfs           = FASTIX_SYSGRP_FS + 25,
    FASTIX_SYS_fstatfs          = FASTIX_SYSGRP_FS + 26,
    FASTIX_SYS_faccessat        = FASTIX_SYSGRP_FS + 27,
    FASTIX_SYS_fchmod           = FASTIX_SYSGRP_FS + 28,
    FASTIX_SYS_fchmodat         = FASTIX_SYSGRP_FS + 29,
    FASTIX_SYS_fchown           = FASTIX_SYSGRP_FS + 30,
    FASTIX_SYS_fchownat         = FASTIX_SYSGRP_FS + 31,
    FASTIX_SYS_utimensat        = FASTIX_SYSGRP_FS + 32,
    FASTIX_SYS_mkdirat          = FASTIX_SYSGRP_FS + 33,
    FASTIX_SYS_mknodat          = FASTIX_SYSGRP_FS + 34,
    FASTIX_SYS_unlinkat         = FASTIX_SYSGRP_FS + 35,
    FASTIX_SYS_renameat2        = FASTIX_SYSGRP_FS + 36,
    FASTIX_SYS_linkat           = FASTIX_SYSGRP_FS + 37,
    FASTIX_SYS_symlinkat        = FASTIX_SYSGRP_FS + 38,
    FASTIX_SYS_readlinkat       = FASTIX_SYSGRP_FS + 39,
    FASTIX_SYS_getdents64       = FASTIX_SYSGRP_FS + 40,
    FASTIX_SYS_getcwd           = FASTIX_SYSGRP_FS + 41,
    FASTIX_SYS_chdir            = FASTIX_SYSGRP_FS + 42,
    FASTIX_SYS_fchdir           = FASTIX_SYSGRP_FS + 43,
    FASTIX_SYS_chroot           = FASTIX_SYSGRP_FS + 44,
    FASTIX_SYS_mount            = FASTIX_SYSGRP_FS + 45,
    FASTIX_SYS_umount2          = FASTIX_SYSGRP_FS + 46,
    FASTIX_SYS_pivot_root       = FASTIX_SYSGRP_FS + 47,
    FASTIX_SYS_sendfile         = FASTIX_SYSGRP_FS + 48,
    FASTIX_SYS_copy_file_range  = FASTIX_SYSGRP_FS + 49,

    /* Sockets & events (netd + unified event broker) */
    FASTIX_SYS_socket           = FASTIX_SYSGRP_NET + 0,
    FASTIX_SYS_socketpair       = FASTIX_SYSGRP_NET + 1,
    FASTIX_SYS_bind             = FASTIX_SYSGRP_NET + 2,
    FASTIX_SYS_listen           = FASTIX_SYSGRP_NET + 3,
    FASTIX_SYS_accept4          = FASTIX_SYSGRP_NET + 4,
    FASTIX_SYS_connect          = FASTIX_SYSGRP_NET + 5,
    FASTIX_SYS_getsockname      = FASTIX_SYSGRP_NET + 6,
    FASTIX_SYS_getpeername      = FASTIX_SYSGRP_NET + 7,
    FASTIX_SYS_sendto           = FASTIX_SYSGRP_NET + 8,
    FASTIX_SYS_recvfrom         = FASTIX_SYSGRP_NET + 9,
    FASTIX_SYS_sendmsg          = FASTIX_SYSGRP_NET + 10,
    FASTIX_SYS_recvmsg          = FASTIX_SYSGRP_NET + 11,
    FASTIX_SYS_setsockopt       = FASTIX_SYSGRP_NET + 12,
    FASTIX_SYS_getsockopt       = FASTIX_SYSGRP_NET + 13,
    FASTIX_SYS_shutdown         = FASTIX_SYSGRP_NET + 14,
    FASTIX_SYS_ppoll            = FASTIX_SYSGRP_NET + 15,
    FASTIX_SYS_pselect6         = FASTIX_SYSGRP_NET + 16,
    FASTIX_SYS_epoll_create1    = FASTIX_SYSGRP_NET + 17,
    FASTIX_SYS_epoll_ctl        = FASTIX_SYSGRP_NET + 18,
    FASTIX_SYS_epoll_pwait      = FASTIX_SYSGRP_NET + 19,

    /* System & misc (procd) */
    FASTIX_SYS_uname            = FASTIX_SYSGRP_SYS + 0,
    FASTIX_SYS_sethostname      = FASTIX_SYSGRP_SYS + 1,
    FASTIX_SYS_setdomainname    = FASTIX_SYSGRP_SYS + 2,
    FASTIX_SYS_getrandom        = FASTIX_SYSGRP_SYS + 3,
    FASTIX_SYS_sysinfo          = FASTIX_SYSGRP_SYS + 4,
    FASTIX_SYS_reboot           = FASTIX_SYSGRP_SYS + 5,
    FASTIX_SYS_syslog           = FASTIX_SYSGRP_SYS + 6,
    FASTIX_SYS_prctl            = FASTIX_SYSGRP_SYS + 7,
    FASTIX_SYS_arch_prctl       = FASTIX_SYSGRP_SYS + 8,
};

/* Each group must stay below the next group's base. */
_Static_assert(FASTIX_SYS_times < FASTIX_SYSGRP_MEM, "proc group overflow");
_Static_assert(FASTIX_SYS_munlock < FASTIX_SYSGRP_SIG, "mem group overflow");
_Static_assert(FASTIX_SYS_signalfd4 < FASTIX_SYSGRP_TIME, "signal group overflow");
_Static_assert(FASTIX_SYS_setpriority < FASTIX_SYSGRP_SYNC, "time group overflow");
_Static_assert(FASTIX_SYS_shmctl < FASTIX_SYSGRP_FS, "sync group overflow");
_Static_assert(FASTIX_SYS_copy_file_range < FASTIX_SYSGRP_NET, "fs group overflow");
_Static_assert(FASTIX_SYS_epoll_pwait < FASTIX_SYSGRP_SYS, "net group overflow");
_Static_assert(FASTIX_SYS_arch_prctl < FASTIX_SYSGRP_END, "sys group overflow");
_Static_assert(FASTIX_SYSGRP_END <= FASTIX_IPC_LABEL_TEST_BASE, "syscalls overlap test labels");
_Static_assert(FASTIX_IPC_LABEL_TEST_BASE < (1 << FASTIX_IPC_LABEL_BITS), "test labels exceed label bits");

#endif /* FASTIX_ABI_H */
