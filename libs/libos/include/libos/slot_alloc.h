/*
 * libos/slot_alloc.h - allocator for free capability slots in a CNode.
 * A bitmap over the slot range [first, first + count); the caller owns the
 * bitmap storage (SLOT_ALLOC_WORDS(count) 64-bit words).
 */
#ifndef LIBOS_SLOT_ALLOC_H
#define LIBOS_SLOT_ALLOC_H

#include <stdint.h>

#include <libos/compiler.h>

#define SLOT_ALLOC_WORDS(count) (((count) + 63) / 64)

struct slot_alloc {
    uint64_t first;  /* first slot index managed */
    uint64_t count;  /* number of slots managed */
    uint64_t used;   /* slots handed out right now */
    uint64_t hint;   /* bitmap word where the next search starts */
    uint64_t *bits;  /* bit set = slot in use */
};

void slot_alloc_init(struct slot_alloc *sa, uint64_t first, uint64_t count, uint64_t *bits);

/* Stores a free slot in *slot. Returns 0, or -FASTIX_ENOMEM if all slots are used. */
LIBOS_MUST_CHECK int slot_alloc_get(struct slot_alloc *sa, uint64_t *slot);

/* Returns a slot. The slot must have been handed out and not yet returned. */
void slot_alloc_put(struct slot_alloc *sa, uint64_t slot);

#endif /* LIBOS_SLOT_ALLOC_H */
