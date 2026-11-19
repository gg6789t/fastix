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
#include <arch/regs.h>
#include <fastix/abi.h>
#include <fastix/cspace.h>
#include <fastix/vdso.h>
#include <fastix/vspace.h>
#include <sel4/sel4.h>

/* Per-thread slot index stored by procd in TPIDRRO_EL0 (0 = main thread). */
static inline uint32_t fastix_tid_slot(void)
{
    return (uint32_t)arch_read_tpidrro();
}

static inline seL4_IPCBuffer *fastix_ipcbuf(void)
{
    uint64_t addr = FASTIX_IPCBUF_BASE + (uint64_t)fastix_tid_slot() * FASTIX_IPCBUF_STRIDE;
    return (seL4_IPCBuffer *)(uintptr_t)addr;
}

static inline seL4_CPtr fastix_procd_ep(void)
{
    return (seL4_CPtr)fastix_cptr_thread_ep(fastix_tid_slot());
}

/* Kept for single-threaded callers that also call libsel4 helpers directly. */
static inline void fastix_sys_init(void)
{
    seL4_SetIPCBuffer(fastix_ipcbuf());
}

#define FASTIX_CALL_MORE 6 /* extra reply words fastix_call can return */

/*
 * Thread-safe call to procd with up to 6 arguments. Uses this thread's own
 * procd endpoint and IPC buffer (from TPIDRRO_EL0), never the global libsel4
 * IPC buffer pointer. Returns MR0 (>= 0 success, -errno failure).
 */
static inline int64_t fastix_call(uint64_t nr, unsigned nargs, const uint64_t *args,
                                  uint64_t more[FASTIX_CALL_MORE])
{
    seL4_IPCBuffer *buf = fastix_ipcbuf();
    seL4_Word mr0 = nargs > 0 ? args[0] : 0;
    seL4_Word mr1 = nargs > 1 ? args[1] : 0;
    seL4_Word mr2 = nargs > 2 ? args[2] : 0;
    seL4_Word mr3 = nargs > 3 ? args[3] : 0;
    for (unsigned i = 4; i < nargs; i++) {
        buf->msg[i] = args[i];
    }

    seL4_Word badge = 0;
    seL4_MessageInfo_t out_info = { { 0 } };
    arm_sys_send_recv(seL4_SysCall, fastix_procd_ep(), &badge,
                      seL4_MessageInfo_new(nr, 0, 0, nargs).words[0], &out_info.words[0],
                      &mr0, &mr1, &mr2, &mr3, 0);

    seL4_Word n = seL4_MessageInfo_get_length(out_info);
    if (more != NULL) {
        seL4_Word reg[4] = { mr0, mr1, mr2, mr3 };
        for (unsigned i = 0; i < FASTIX_CALL_MORE; i++) {
            unsigned idx = i + 1;
            if (idx >= n) {
                more[i] = 0;
            } else if (idx < 4) {
                more[i] = reg[idx];
            } else {
                more[i] = buf->msg[idx];
            }
        }
    }
    return (int64_t)mr0;
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

static inline int64_t fastix_sys4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    uint64_t a[4] = { a0, a1, a2, a3 };
    return fastix_call(nr, 4, a, NULL);
}

static inline int64_t fastix_sys5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4)
{
    uint64_t a[5] = { a0, a1, a2, a3, a4 };
    return fastix_call(nr, 5, a, NULL);
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

/* CLOCK_REALTIME in nanoseconds, read from the VDSO page with no IPC. */
static inline uint64_t fastix_clock_realtime_ns(void)
{
    struct fastix_vdso_data *d = (struct fastix_vdso_data *)FASTIX_VDSO_ADDR;

    for (;;) {
        uint32_t seq = atomic_load_explicit(&d->seq, memory_order_acquire);
        if (seq & 1) {
            continue;
        }
        uint64_t ns = fastix_vdso_mono_ns(d, arch_read_counter()) + d->realtime_offset_ns;
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
