/*
 * futex.h - userspace futex waitqueues (private & shared keys, bitsets,
 * requeue, timeouts) and robust list cleanup on thread exit.
 */
#ifndef PROCD_FUTEX_H
#define PROCD_FUTEX_H

#include <stdbool.h>
#include <stdint.h>

#include "proc.h"

void futex_module_init(void);

/* Removes `t` from its futex waitqueue if parked in PARK_FUTEX. */
void futex_cancel_wait(struct thread *t);

/* Wakes any PARK_FUTEX waiters whose timeout expired (called from timer_tick). */
void futex_tick(void);

/* Wakes up to `nr` waiters on `uaddr` (used by clear_child_tid & robust list). */
int futex_wake_internal(struct vmspace *vs, uint64_t uaddr, uint32_t nr, bool priv,
                        uint32_t bitset);

/* Walks `t->robust_list` on thread exit, setting OWNER_DIED and waking waiters. */
void futex_exit_robust_list(struct thread *t);

/*
 * FASTIX_SYS_futex(uaddr, op, val, val2_or_timeout, uaddr2, val3).
 * Sets *park = true if `t` must be parked in PARK_FUTEX; otherwise returns >= 0 or -errno.
 */
int futex_sys_op(struct thread *t, uint64_t uaddr, uint32_t op, uint32_t val,
                 uint64_t val2, uint64_t uaddr2, uint32_t val3, bool *park);

int futex_sys_set_robust_list(struct thread *t, uint64_t head_u, uint64_t len);
int futex_sys_get_robust_list(struct thread *caller, uint32_t pid, uint64_t head_ptr_u,
                              uint64_t len_ptr_u);

#endif /* PROCD_FUTEX_H */
