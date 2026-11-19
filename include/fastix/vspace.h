/*
 * fastix/vspace.h - the user virtual address layout (AGENTS.md 5.3).
 * Same layout on AArch64 and x86_64; everything sits below 2^47.
 */
#ifndef FASTIX_VSPACE_H
#define FASTIX_VSPACE_H

#include <fastix/config.h>

/* Addresses below this are never mapped, so NULL+offset faults. */
#define FASTIX_NULL_GUARD_END     0x0000000000100000ULL   /* 1 MB */

/* Static / non-PIE executables. */
#define FASTIX_EXEC_BASE          0x0000000000400000ULL
#define FASTIX_EXEC_END           0x0000000040000000ULL

/* PIE executables and the dynamic linker. */
#define FASTIX_PIE_BASE           0x0000000100000000ULL

/* mmap region. Ends 4 GB below the stack top, leaving room for the
 * main stack to grow (default 8 MB, raisable with setrlimit). */
#define FASTIX_MMAP_BASE          0x0000001000000000ULL
#define FASTIX_MMAP_END           0x0000006F00000000ULL

/* Main thread stack: top address, grows down. */
#define FASTIX_STACK_TOP          0x0000007000000000ULL

/* Read-only pages shared with every process. */
#define FASTIX_VDSO_ADDR          0x0000007FF0000000ULL   /* clock data (R--) */
#define FASTIX_VDSO_CODE_ADDR     (FASTIX_VDSO_ADDR + FASTIX_PAGE_SIZE) /* sigreturn trampoline (R-X) */
#define FASTIX_PROCINFO_ADDR      0x0000007FF0010000ULL   /* pid, uid, ... (read-only) */

/* Per-thread seL4 IPC buffer: one page each, indexed by tid slot. */
#define FASTIX_IPCBUF_BASE        0x0000007FF1000000ULL
#define FASTIX_IPCBUF_STRIDE      FASTIX_PAGE_SIZE

/* Per-thread shared I/O window, indexed by tid slot (all windows: one region). */
#define FASTIX_IOWIN_BASE         0x0000007FF2000000ULL
#define FASTIX_IOWIN_STRIDE       FASTIX_SHM_IO_WINDOW
#define FASTIX_IOWIN_REGION \
    ((unsigned long long)FASTIX_MAX_THREADS_PER_PROC * FASTIX_IOWIN_STRIDE)

/* First address above everything Fastix maps for a user process. */
#define FASTIX_VSPACE_USER_END    (FASTIX_IOWIN_BASE + FASTIX_IOWIN_REGION)

/* Compile-time proof that the regions are in order and never overlap. */
_Static_assert(FASTIX_NULL_GUARD_END <= FASTIX_EXEC_BASE, "null guard overlaps exec");
_Static_assert(FASTIX_EXEC_END <= FASTIX_PIE_BASE, "exec overlaps PIE base");
_Static_assert(FASTIX_PIE_BASE < FASTIX_MMAP_BASE, "PIE base overlaps mmap");
_Static_assert(FASTIX_MMAP_END + FASTIX_DEFAULT_STACK_SIZE <= FASTIX_STACK_TOP,
               "mmap region overlaps default stack");
_Static_assert(FASTIX_STACK_TOP <= FASTIX_VDSO_ADDR, "stack overlaps VDSO");
_Static_assert(FASTIX_VDSO_ADDR + 2 * FASTIX_PAGE_SIZE <= FASTIX_PROCINFO_ADDR,
               "VDSO overlaps procinfo");
_Static_assert(FASTIX_PROCINFO_ADDR + FASTIX_PAGE_SIZE <= FASTIX_IPCBUF_BASE,
               "procinfo overlaps IPC buffers");
_Static_assert(FASTIX_IPCBUF_BASE + (unsigned long long)FASTIX_MAX_THREADS_PER_PROC *
               FASTIX_IPCBUF_STRIDE <= FASTIX_IOWIN_BASE, "IPC buffers overlap I/O windows");
_Static_assert(FASTIX_VSPACE_USER_END <= 0x0000800000000000ULL, "layout exceeds 47-bit VA");

#endif /* FASTIX_VSPACE_H */
