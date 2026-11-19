/*
 * kobj.h - procd's kernel object allocator.
 * Each object gets its own untyped block from the buddy allocator, so freeing
 * an object revokes that block: the kernel deletes the object and every cap
 * derived from it (copies, mints, mappings), and the memory is zeroed on reuse.
 */
#ifndef PROCD_KOBJ_H
#define PROCD_KOBJ_H

#include <stdint.h>

#include <libos/compiler.h>
#include <libos/kalloc.h>
#include <libos/ut_buddy.h>
#include <sel4/sel4.h>

/* Totals used to prove nothing leaked: equal before and after a create/destroy cycle. */
struct kobj_stats {
    uint64_t free_bytes;    /* free untyped memory */
    uint64_t blocks_in_use; /* buddy blocks (free + split + used) being tracked */
    uint64_t slots_used;    /* procd CNode slots in use */
};

/* Takes over the RAM untypeds (except `skip_untyped`) and the free slots
 * [first_slot, first_slot + num_slots). Returns 0 or -errno. */
LIBOS_MUST_CHECK int kobj_init(const seL4_BootInfo *bi, seL4_Word skip_untyped,
                               seL4_CPtr first_slot, seL4_Word num_slots);

/* Creates one object. `param` is the size argument for variable-size types
 * (CNode radix bits, SchedContext size bits, untyped bits); 0 otherwise. */
LIBOS_MUST_CHECK int kobj_alloc(seL4_Word type, seL4_Word param, struct kobj *out);

/* Destroys the object and everything derived from it. */
void kobj_free(struct kobj *obj);

/* Plain slots in procd's CNode for caps that are not objects (copies, mints). */
LIBOS_MUST_CHECK int kobj_slot_get(seL4_CPtr *slot);
void kobj_slot_put(seL4_CPtr slot); /* slot must already be empty */

struct kobj_stats kobj_stats(void);

/* Maps a seL4 error to -errno. */
int kobj_sel4_err(seL4_Error err);

#endif /* PROCD_KOBJ_H */
