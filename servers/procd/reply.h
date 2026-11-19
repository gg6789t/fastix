/*
 * reply.h - pool of seL4 Reply objects (MCS). procd receives every call with
 * a Reply object; when a call must wait (e.g. wait4 with no exited child),
 * that Reply object is parked with the caller and the server loop takes a
 * fresh one, so procd keeps serving others (AGENTS.md 5.5).
 */
#ifndef PROCD_REPLY_H
#define PROCD_REPLY_H

#include <stdint.h>

#include <libos/compiler.h>
#include <libos/list.h>

#include "kobj.h"

struct reply_obj {
    struct list_node link; /* on the free list while unused */
    struct kobj obj;
};

void reply_module_init(void);

/* Returns 0 with a Reply object (reused or new), or -errno. */
LIBOS_MUST_CHECK int reply_get(struct reply_obj **out);

/* Returns a Reply object that is no longer bound to a caller. */
void reply_put(struct reply_obj *r);

/* Replies through a parked Reply object with words mr[0..n-1], then returns it to the pool. */
void reply_send(struct reply_obj *r, unsigned n, const uint64_t *mr);

#endif /* PROCD_REPLY_H */
