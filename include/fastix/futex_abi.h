/*
 * fastix/futex_abi.h - futex operation codes, thread_create flags, and robust
 * list structures. Values match Linux/musl AArch64 so libc needs no translation.
 */
#ifndef FASTIX_FUTEX_ABI_H
#define FASTIX_FUTEX_ABI_H

#include <stdint.h>

/* thread_create flags (subset of Linux clone flags used by musl __clone) */
#define FASTIX_CLONE_SETTLS         0x00080000u
#define FASTIX_CLONE_CHILD_CLEARTID 0x00200000u
#define FASTIX_CLONE_CHILD_SETTID   0x01000000u

/* Futex operations (low 7 bits) & modifier flags */
#define FASTIX_FUTEX_WAIT            0
#define FASTIX_FUTEX_WAKE            1
#define FASTIX_FUTEX_REQUEUE         3
#define FASTIX_FUTEX_CMP_REQUEUE     4
#define FASTIX_FUTEX_WAIT_BITSET     9
#define FASTIX_FUTEX_WAKE_BITSET     10

#define FASTIX_FUTEX_CMD_MASK        0x7Fu
#define FASTIX_FUTEX_PRIVATE_FLAG    128u
#define FASTIX_FUTEX_CLOCK_REALTIME  256u

#define FASTIX_FUTEX_BITSET_MATCH_ANY 0xFFFFFFFFu

/* Robust futex word bits */
#define FASTIX_FUTEX_WAITERS         0x80000000u
#define FASTIX_FUTEX_OWNER_DIED      0x40000000u
#define FASTIX_FUTEX_TID_MASK        0x3FFFFFFFu

#define FASTIX_ROBUST_LIST_LIMIT     1024

struct fastix_robust_list {
    uint64_t next;
};

struct fastix_robust_list_head {
    struct fastix_robust_list list;
    int64_t futex_offset;
    uint64_t list_op_pending;
};

#endif /* FASTIX_FUTEX_ABI_H */
