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
#define FASTIX_PROCD_CSPACE_L1_BITS  8           /* procd CSpace level 1: 256 entries */
#define FASTIX_PROCD_CSPACE_L2_COUNT 16          /* extra level-2 CNodes (boot CNode size each) */
#define FASTIX_PROCD_MAX_SLOTS       65536       /* slots procd allocates (L2_COUNT x 4096) */
#define FASTIX_PROCD_UT_NODES        2048        /* untyped blocks tracked by the buddy allocator */
#define FASTIX_FRAME_CHUNK_BITS      21          /* user frames are made in 2 MB chunks */
#define FASTIX_PROCD_OWN_PTS         64          /* page tables for procd's own mappings */
#define FASTIX_PROCD_SCRATCH_VADDR   0x0000004000000000  /* 2 pages: procd window for filling/copying frames */
#define FASTIX_PROCD_VDSO_VADDR      0x0000004000200000  /* procd's writable view of the VDSO data page */
#define FASTIX_PROCD_PIPE_VADDR      0x0000004100000000  /* procd views of active pipe rings (64 x 128 KB) */
#define FASTIX_PROCD_MAX_PIPES       64
#define FASTIX_PROCD_HEAP_VADDR      0x0000005000000000  /* procd heap (slab pages), grows up */
#define FASTIX_PROCD_HEAP_MAX        134217728   /* 128 MB */

/* Binary size budgets in bytes of .text+.rodata (AGENTS.md 14.4) */
#define FASTIX_BUDGET_PROCD          196608      /* 192 KB */
#define FASTIX_BUDGET_VFSD           163840      /* 160 KB */
#define FASTIX_BUDGET_NETD           98304       /*  96 KB */
#define FASTIX_BUDGET_TTYD           32768       /*  32 KB */
#define FASTIX_BUDGET_DEVD           32768       /*  32 KB */
#define FASTIX_BUDGET_DRIVER         32768       /*  32 KB each */

#endif /* FASTIX_CONFIG_H */
