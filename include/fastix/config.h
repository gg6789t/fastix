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
#define FASTIX_ARG_MAX               131072      /* 128 KB: argv + envp strings and pointers */
#define FASTIX_EXEC_MAX_ARGS         4096        /* argv entries (and envp entries) per execve */

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
#define FASTIX_PROCD_OWN_PTS         128         /* page tables for procd's own mappings */
#define FASTIX_PROCD_SCRATCH_VADDR   0x0000004000000000  /* 2 pages: procd window for filling/copying frames */
#define FASTIX_PROCD_VDSO_VADDR      0x0000004000200000  /* procd's writable view of the VDSO data page */
#define FASTIX_PROCD_PIPE_VADDR      0x0000004100000000  /* procd views of active pipe rings (64 x 128 KB) */
#define FASTIX_PROCD_FDEXP_VADDR     0x0000004200000000  /* procd read-only views of vfsd's FD tables (4 KB per client) */
#define FASTIX_PROCD_MAX_PIPES       64
#define FASTIX_PROCD_MAX_FDOBJS      1024        /* pipe-end + eventfd objects (index 0 unused) */
#define FASTIX_PROCD_HEAP_VADDR      0x0000005000000000  /* procd heap (slab pages), grows up */
#define FASTIX_PROCD_HEAP_MAX        134217728   /* 128 MB */

/* devd and isolated driver limits. */
#define FASTIX_DEVD_CSPACE_BITS      12          /* 4096 slots for devd */
#define FASTIX_DEVD_RAM_UT_BITS      22          /* 4 MB RAM untyped granted to devd */
#define FASTIX_DEVD_MAX_DEV_UTS      32          /* max device untypeds passed from BootInfo */
#define FASTIX_DEVD_UT_NODES         512         /* buddy allocator nodes inside devd */
#define FASTIX_DEVD_STACK_SIZE       16384       /* 16 KB devd stack */
#define FASTIX_DEVD_PRIO             (FASTIX_DEFAULT_PRIO + 2)
#define FASTIX_VFSD_PRIO             (FASTIX_DEFAULT_PRIO + 1)
#define FASTIX_DRV_CNODE_BITS        6           /* 64 slots per isolated driver */
#define FASTIX_DRV_STACK_SIZE        16384       /* 16 KB stack per isolated driver */
#define FASTIX_DRV_PRIO              (FASTIX_DEFAULT_PRIO + 1)
#define FASTIX_DRV_MAX_RESTARTS      16          /* restart cap before giving up on a driver */
#define FASTIX_BLK_SECTOR_SIZE       512         /* standard block device sector size */

/* Binary size budgets in bytes of .text+.rodata (AGENTS.md 14.4) */
#define FASTIX_BUDGET_PROCD          196608      /* 192 KB */
#define FASTIX_BUDGET_VFSD           163840      /* 160 KB */
#define FASTIX_BUDGET_NETD           98304       /*  96 KB */
#define FASTIX_BUDGET_TTYD           32768       /*  32 KB */
#define FASTIX_BUDGET_DEVD           32768       /*  32 KB */
#define FASTIX_BUDGET_DRIVER         32768       /*  32 KB each */

#endif /* FASTIX_CONFIG_H */
