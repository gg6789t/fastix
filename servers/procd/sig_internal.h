/*
 * sig_internal.h - helpers shared by signal.c and sig_sys.c only.
 * Not part of procd's public signal API (signal.h).
 */
#ifndef PROCD_SIG_INTERNAL_H
#define PROCD_SIG_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <fastix/signal_abi.h>
#include <libos/list.h>
#include <sel4/sel4.h>

#include "proc.h"

/* SIGKILL and SIGSTOP can never be caught, blocked or ignored. */
#define UNCATCHABLE_MASK (fastix_sig_bit(FASTIX_SIGKILL) | fastix_sig_bit(FASTIX_SIGSTOP))

/* What happens to a signal with SIG_DFL. */
enum def_action {
    DEF_TERM,
    DEF_IGN,
    DEF_STOP,
    DEF_CONT,
};

bool sig_valid(int sig);
enum def_action sig_default_action(int sig);

/* Drops queued signals in `drop_bits` from queue `q` and its pending mask and count. */
void sig_discard_mask(struct list_node *q, uint64_t *mask, uint32_t *count, uint64_t drop_bits);

/* Lowest pending, unblocked signal of `t`, or 0. */
int sig_next_deliverable(const struct thread *t);

/* True if `sp` is on t's alternate signal stack. */
bool sig_on_altstack(const struct thread *t, uint64_t sp);

/* Delivers one pending signal to suspended `t` using `regs`; false if none was deliverable. */
bool sig_deliver_pending(struct thread *t, const seL4_UserContext *regs);

#endif /* PROCD_SIG_INTERNAL_H */
