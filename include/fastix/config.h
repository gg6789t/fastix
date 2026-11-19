/*
 * fastix/config.h - global limits and sizes (AGENTS.md 14.1, 14.4).
 * Plain integer #defines only, so assembly files can include it too.
 */
#ifndef FASTIX_CONFIG_H
#define FASTIX_CONFIG_H

/* Pages */
#define FASTIX_PAGE_SHIFT            12
#define FASTIX_PAGE_SIZE             4096
#define FASTIX_SUPERPAGE_SIZE        2097152     /* 2 MB */

/* Processes and threads */
#define FASTIX_MAX_PROCS             4096
#define FASTIX_PID_MAX               32767
#define FASTIX_MAX_THREADS_PER_PROC  1024

/* Files and paths */
#define FASTIX_DEFAULT_NOFILE        1024
#define FASTIX_MAX_NOFILE            65536
#define FASTIX_PATH_MAX              4096
#define FASTIX_NAME_MAX              255
#define FASTIX_SYMLOOP_MAX           40
#define FASTIX_ARG_MAX               131072      /* 128 KB */

/* Pipes and shared I/O */
#define FASTIX_PIPE_BUF              4096        /* POSIX atomic write size */
#define FASTIX_PIPE_RING_SIZE        65536       /* 64 KB */
#define FASTIX_SHM_IO_WINDOW         65536       /* 64 KB per thread */

/* Stacks */
#define FASTIX_DEFAULT_STACK_SIZE    8388608     /* 8 MB main thread stack */
#define FASTIX_THREAD_STACK_DEFAULT  131072      /* 128 KB pthread stack */
#define FASTIX_SERVER_STACK_SIZE     32768       /* 32 KB per server thread */

/* Signals and FD passing */
#define FASTIX_SIGQUEUE_MAX          64
#define FASTIX_SCM_MAX_FD            253
#define FASTIX_UNIX_INFLIGHT_MAX     4096

/* Scheduling defaults for user threads (MCS budget = period = full CPU share). */
#define FASTIX_DEFAULT_PRIO          100
#define FASTIX_DEFAULT_TIMESLICE_US  5000

/* procd internal limits and its own address-space layout (not visible to users). */
#define FASTIX_PROCD_MAX_SLOTS       4096        /* procd root CNode slots it manages */
#define FASTIX_PROCD_UT_NODES        2048        /* untyped blocks tracked by the buddy allocator */
#define FASTIX_PROCD_MAX_PROC_OBJS   64          /* kernel objects per process (Phase 1 bound) */
#define FASTIX_PROCD_SCRATCH_VADDR   0x0000004000000000  /* procd window for filling frames */

/* Binary size budgets in bytes of .text+.rodata (AGENTS.md 14.4) */
#define FASTIX_BUDGET_PROCD          196608      /* 192 KB */
#define FASTIX_BUDGET_VFSD           163840      /* 160 KB */
#define FASTIX_BUDGET_NETD           98304       /*  96 KB */
#define FASTIX_BUDGET_TTYD           32768       /*  32 KB */
#define FASTIX_BUDGET_DEVD           32768       /*  32 KB */
#define FASTIX_BUDGET_DRIVER         32768       /*  32 KB each */

#endif /* FASTIX_CONFIG_H */
