/*
 * fastix_sys/sys.h - lowest-level user-side calls into Fastix (AGENTS.md 7.2).
 * IPC syscalls to procd, plus zero-IPC reads of the VDSO clock and the
 * per-process info page. musl will be built on top of this in Phase 5.
 */
#ifndef FASTIX_SYS_SYS_H
#define FASTIX_SYS_SYS_H

#include <stdatomic.h>
#include <stdint.h>

#include <arch/counter.h>
#include <fastix/abi.h>
#include <fastix/cspace.h>
#include <fastix/vdso.h>
#include <fastix/vspace.h>
#include <sel4/sel4.h>

/* Must run first in every thread: tells libsel4 where this thread's IPC buffer is. */
static inline void fastix_sys_init(void)
{
    seL4_SetIPCBuffer((seL4_IPCBuffer *)FASTIX_IPCBUF_BASE);
}

#define FASTIX_CALL_MORE 6 /* extra reply words fastix_call can return */

/*
 * Calls procd with up to 6 arguments. Returns MR0 (>= 0 success, -errno failure).
 * If `more` is not NULL it receives reply words MR1..MR6 (0 when absent).
 */
static inline int64_t fastix_call(uint64_t nr, unsigned nargs, const uint64_t *args,
                                  uint64_t more[FASTIX_CALL_MORE])
{
    for (unsigned i = 0; i < nargs; i++) {
        seL4_SetMR((int)i, args[i]);
    }
    seL4_MessageInfo_t info =
        seL4_Call(FASTIX_CAP_PROCD_EP_MAIN, seL4_MessageInfo_new(nr, 0, 0, nargs));
    seL4_Word n = seL4_MessageInfo_get_length(info);
    if (more != NULL) {
        for (unsigned i = 0; i < FASTIX_CALL_MORE; i++) {
            more[i] = (i + 1 < n) ? seL4_GetMR((int)i + 1) : 0;
        }
    }
    return (int64_t)seL4_GetMR(0);
}

static inline int64_t fastix_sys0(uint64_t nr)
{
    return fastix_call(nr, 0, NULL, NULL);
}

static inline int64_t fastix_sys1(uint64_t nr, uint64_t a0)
{
    uint64_t a[1] = { a0 };
    return fastix_call(nr, 1, a, NULL);
}

static inline int64_t fastix_sys2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    uint64_t a[2] = { a0, a1 };
    return fastix_call(nr, 2, a, NULL);
}

static inline int64_t fastix_sys3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    uint64_t a[3] = { a0, a1, a2 };
    return fastix_call(nr, 3, a, NULL);
}

static inline int64_t fastix_sys6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t a[6] = { a0, a1, a2, a3, a4, a5 };
    return fastix_call(nr, 6, a, NULL);
}

/* CLOCK_MONOTONIC in nanoseconds, read from the VDSO page with no IPC. */
static inline uint64_t fastix_clock_mono_ns(void)
{
    /* Non-const pointer: C11 atomic loads need one, even though the page is read-only. */
    struct fastix_vdso_data *d = (struct fastix_vdso_data *)FASTIX_VDSO_ADDR;

    for (;;) {
        uint32_t seq = atomic_load_explicit(&d->seq, memory_order_acquire);
        if (seq & 1) {
            continue; /* procd is updating the page */
        }
        uint64_t ns = fastix_vdso_mono_ns(d, arch_read_counter());
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&d->seq, memory_order_relaxed) == seq) {
            return ns;
        }
    }
}

/* This process's info page (pid, ppid, credentials), read with no IPC. */
static inline const struct fastix_procinfo *fastix_procinfo(void)
{
    return (const struct fastix_procinfo *)FASTIX_PROCINFO_ADDR;
}

#endif /* FASTIX_SYS_SYS_H */
