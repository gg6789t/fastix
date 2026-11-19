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
#define FASTIX_CAP_RESERVED_FIRST   6   /* 6..15 reserved for future servers */
#define FASTIX_CAP_RESERVED_LAST    15
#define FASTIX_CAP_RINGS_L1_FIRST   16  /* 16..47 first 32 per-FD ring notifications */
#define FASTIX_CAP_RINGS_L1_LAST    47
#define FASTIX_CAP_RINGS_CNODE      62  /* level-2 CNode for more ring notifications */
#define FASTIX_CAP_THREAD_CNODE     63  /* level-2 CNode for per-thread caps */

/* Level-2 CNode sizes (allocated lazily). */
#define FASTIX_RINGS_CNODE_BITS     10  /* 1024 more ring notifications */
#define FASTIX_THREAD_CNODE_BITS    11  /* 2 slots per thread x 1024 threads */

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
