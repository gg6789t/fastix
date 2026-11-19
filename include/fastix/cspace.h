/*
 * fastix/cspace.h - fixed capability slots of every user process and the
 * badge encoding used by procd (AGENTS.md 5.4, 14.2).
 */
#ifndef FASTIX_CSPACE_H
#define FASTIX_CSPACE_H

#include <stdint.h>

/* Root CNode of every process: 2^6 = 64 slots. Never resized. */
#define FASTIX_ROOT_CNODE_BITS      6

/* Fixed root CNode slots. */
#define FASTIX_CAP_NULL             0   /* invalid sentinel */
#define FASTIX_CAP_PROCD_EP_MAIN    1   /* main thread -> procd */
#define FASTIX_CAP_VFSD_EP          2   /* -> vfsd */
#define FASTIX_CAP_NETD_EP          3   /* -> netd */
#define FASTIX_CAP_TTYD_EP          4   /* -> ttyd */
#define FASTIX_CAP_MAIN_NTFN        5   /* main thread wakeups */
#define FASTIX_CAP_DEVD_EP          6   /* -> devd (PID 1 / system servers only) */
#define FASTIX_CAP_DRV_FIRST        7   /* 7..11 test/server driver service endpoints */
#define FASTIX_CAP_DRV_LAST         11
#define FASTIX_CAP_SERVER_EP        12  /* system servers only: own service endpoint */
#define FASTIX_CAP_SERVER_PROCD     13  /* system servers only: procd server-control endpoint */
#define FASTIX_CAP_SERVER_REPLY     14  /* system servers only: MCS reply object */
#define FASTIX_CAP_RESERVED_FIRST   15  /* 15 reserved for future servers */
#define FASTIX_CAP_RESERVED_LAST    15
#define FASTIX_CAP_RINGS_L1_FIRST   16  /* 16..47 first 32 per-FD ring notifications */
#define FASTIX_CAP_RINGS_L1_LAST    47
#define FASTIX_CAP_RINGS_CNODE      62  /* level-2 CNode for more ring notifications */
#define FASTIX_CAP_THREAD_CNODE     63  /* level-2 CNode for per-thread caps */

/* Level-2 CNode sizes (allocated lazily). */
#define FASTIX_RINGS_CNODE_BITS     10  /* 1024 more ring notifications */
#define FASTIX_THREAD_CNODE_BITS    11  /* 2 slots per thread x 1024 threads */
#define FASTIX_CSPACE_L2_BITS       FASTIX_THREAD_CNODE_BITS /* max level-2 depth */

/*
 * Two-level user CSpace CPtr helpers (AGENTS.md 5.4).
 * The root CNode (6 bits) uses guard = 64 - 6 - 11 = 47 zero bits, so:
 *   - Level-1 slots 0..61 resolve with CPtr = (slot << 11) (non-CNode caps stop early).
 *   - Level-2 THREAD_CNODE at slot 63 (11 bits, guard 0) resolves (63 << 11) | sub.
 *   - Level-2 RINGS_CNODE at slot 62 (10 bits, guard 1 bit of 0) resolves (62 << 11) | sub.
 */
static inline uint64_t fastix_cptr_l1(uint32_t slot)
{
    return (uint64_t)slot << FASTIX_CSPACE_L2_BITS;
}

static inline uint64_t fastix_cptr_thread_ep(uint32_t tid_slot)
{
    if (tid_slot == 0) {
        return fastix_cptr_l1(FASTIX_CAP_PROCD_EP_MAIN);
    }
    return ((uint64_t)FASTIX_CAP_THREAD_CNODE << FASTIX_CSPACE_L2_BITS) |
           ((uint64_t)tid_slot * 2u);
}

static inline uint64_t fastix_cptr_thread_ntfn(uint32_t tid_slot)
{
    if (tid_slot == 0) {
        return fastix_cptr_l1(FASTIX_CAP_MAIN_NTFN);
    }
    return ((uint64_t)FASTIX_CAP_THREAD_CNODE << FASTIX_CSPACE_L2_BITS) |
           ((uint64_t)tid_slot * 2u + 1u);
}

static inline uint64_t fastix_cptr_ring(uint32_t ring_idx)
{
    uint32_t l1_count = FASTIX_CAP_RINGS_L1_LAST - FASTIX_CAP_RINGS_L1_FIRST + 1u;
    if (ring_idx < l1_count) {
        return fastix_cptr_l1(FASTIX_CAP_RINGS_L1_FIRST + ring_idx);
    }
    return ((uint64_t)FASTIX_CAP_RINGS_CNODE << FASTIX_CSPACE_L2_BITS) |
           (uint64_t)(ring_idx - l1_count);
}

/* procd badge layout (64 bits): type[63:60] pid[59:32] tid_slot[31:16] gen[15:0]. */
#define FASTIX_BADGE_SYSCALL        0x1u  /* user syscall */
#define FASTIX_BADGE_FAULT          0x2u  /* user fault */
#define FASTIX_BADGE_SERVER         0x3u  /* core server control */
#define FASTIX_BADGE_NOTIFY         0x4u  /* async notification */

#define FASTIX_BADGE_TYPE_SHIFT     60
#define FASTIX_BADGE_PID_SHIFT      32
#define FASTIX_BADGE_TID_SHIFT      16
#define FASTIX_BADGE_PID_MASK       0x0FFFFFFFu   /* 28 bits */
#define FASTIX_BADGE_TID_MASK       0xFFFFu       /* 16 bits */
#define FASTIX_BADGE_GEN_MASK       0xFFFFu       /* 16 bits */

static inline uint64_t fastix_badge_make(uint32_t type, uint32_t pid, uint32_t tid_slot,
                                         uint32_t gen)
{
    return ((uint64_t)(type & 0xFu) << FASTIX_BADGE_TYPE_SHIFT) |
           ((uint64_t)(pid & FASTIX_BADGE_PID_MASK) << FASTIX_BADGE_PID_SHIFT) |
           ((uint64_t)(tid_slot & FASTIX_BADGE_TID_MASK) << FASTIX_BADGE_TID_SHIFT) |
           (uint64_t)(gen & FASTIX_BADGE_GEN_MASK);
}

static inline uint32_t fastix_badge_type(uint64_t badge)
{
    return (uint32_t)(badge >> FASTIX_BADGE_TYPE_SHIFT);
}

static inline uint32_t fastix_badge_pid(uint64_t badge)
{
    return (uint32_t)(badge >> FASTIX_BADGE_PID_SHIFT) & FASTIX_BADGE_PID_MASK;
}

static inline uint32_t fastix_badge_tid_slot(uint64_t badge)
{
    return (uint32_t)(badge >> FASTIX_BADGE_TID_SHIFT) & FASTIX_BADGE_TID_MASK;
}

static inline uint32_t fastix_badge_gen(uint64_t badge)
{
    return (uint32_t)badge & FASTIX_BADGE_GEN_MASK;
}

#endif /* FASTIX_CSPACE_H */
