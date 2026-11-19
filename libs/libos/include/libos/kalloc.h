/*
 * libos/kalloc.h - kernel object allocator shared by servers (procd, devd).
 * Each object gets its own untyped block from a ut_buddy, so freeing an object
 * revokes that block: the kernel deletes the object and every cap derived from
 * it, and the memory is zeroed when reused. The server supplies only what
 * depends on its CSpace layout: the root CNode and how to retype into a slot.
 */
#ifndef LIBOS_KALLOC_H
#define LIBOS_KALLOC_H

#include <stddef.h>
#include <stdint.h>

#include <libos/compiler.h>
#include <libos/slot_alloc.h>
#include <libos/ut_buddy.h>
#include <sel4/sel4.h>

struct kobj {
    struct ut_block *block; /* untyped block the object was made from (NULL if none) */
    seL4_CPtr cap;          /* object cap in the server's CSpace */
};

/* Creates one object of `type` from `untyped` in the server's slot `slot`. */
typedef seL4_Error (*kalloc_retype_fn)(seL4_CPtr untyped, seL4_Word type, seL4_Word size_bits,
                                       seL4_CPtr slot);

struct kalloc {
    struct ut_buddy buddy;
    struct slot_alloc *slots;
    seL4_CPtr root;          /* CNode used for revoke with depth seL4_WordBits */
    kalloc_retype_fn retype;
};

/* `nodes` is storage for up to `n` untyped blocks; untypeds are added with ut_buddy_add(&ka->buddy, ...). */
void kalloc_init(struct kalloc *ka, struct ut_block *nodes, size_t n, struct slot_alloc *slots,
                 seL4_CPtr root, kalloc_retype_fn retype);

/* Maps a seL4 error to 0 or -errno. */
int kalloc_sel4_err(seL4_Error err);

/* Size in bits of an object (`param`: CNode radix, SchedContext/untyped size bits), 0 if unsupported. */
unsigned kalloc_object_bits(seL4_Word type, seL4_Word param);

/* Creates one object. Returns 0 or -errno. */
LIBOS_MUST_CHECK int kalloc_obj(struct kalloc *ka, seL4_Word type, seL4_Word param,
                                struct kobj *out);

/* Destroys the object and everything derived from it (no-op if obj->block is NULL). */
void kalloc_free(struct kalloc *ka, struct kobj *obj);

#endif /* LIBOS_KALLOC_H */
