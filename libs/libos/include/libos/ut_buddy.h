/*
 * libos/ut_buddy.h - buddy allocator over seL4 untyped memory capabilities.
 *
 * Every block is an untyped cap of 2^bits bytes. Allocating splits a larger
 * free block in halves (retyping two child untypeds) until the size fits.
 * Freeing revokes the block, which deletes every kernel object made from it,
 * then merges it with its buddy (by revoking the parent) while both are free.
 * The seL4 calls are behind `struct ut_backend` so host tests can use a model.
 */
#ifndef LIBOS_UT_BUDDY_H
#define LIBOS_UT_BUDDY_H

#include <stddef.h>
#include <stdint.h>

#include <libos/compiler.h>
#include <libos/list.h>
#include <libos/slot_alloc.h>

#define UT_MIN_BITS 4    /* smallest untyped seL4 allows (seL4_MinUntypedBits) */
#define UT_MAX_BITS 47   /* largest untyped on 64-bit Arm (seL4_MaxUntypedBits) */

struct ut_backend {
    /* Retype one untyped of 2^bits bytes from `parent` into empty slot `slot`. Returns 0 or -errno. */
    int (*split)(void *ctx, uint64_t parent, uint64_t slot, unsigned bits);
    /* Delete every capability derived from `parent`. Returns 0 or -errno. */
    int (*revoke)(void *ctx, uint64_t parent);
    void *ctx;
};

enum ut_state { UT_FREE, UT_SPLIT, UT_USED };

struct ut_block {
    struct list_node link;     /* free list (UT_FREE) or spare node list */
    struct ut_block *parent;   /* NULL for a root block given at boot */
    struct ut_block *child[2]; /* halves, while UT_SPLIT */
    uint64_t slot;             /* CNode slot of this block's untyped cap */
    uint64_t paddr;            /* physical address */
    uint8_t bits;              /* size is 2^bits bytes */
    uint8_t state;             /* enum ut_state */
};

struct ut_buddy {
    struct list_node free_list[UT_MAX_BITS + 1]; /* free blocks by size */
    struct list_node spare;                      /* unused ut_block structs */
    struct slot_alloc *slots;                    /* slots for child untypeds */
    struct ut_backend be;
    uint64_t free_bytes;    /* bytes in UT_FREE blocks */
    uint64_t blocks_in_use; /* ut_block structs not on the spare list */
};

/* `nodes` is caller storage for up to `n` blocks (roots and split children). */
void ut_buddy_init(struct ut_buddy *ub, struct ut_block *nodes, size_t n,
                   struct slot_alloc *slots, struct ut_backend be);

/* Adds a boot untyped (an empty, unsplit cap already in `slot`). Returns 0 or -FASTIX_ENOMEM. */
LIBOS_MUST_CHECK int ut_buddy_add(struct ut_buddy *ub, uint64_t slot, uint64_t paddr,
                                  unsigned bits);

/* Allocates a block of exactly 2^bits bytes (bits >= UT_MIN_BITS). Returns 0 or -errno. */
LIBOS_MUST_CHECK int ut_buddy_alloc(struct ut_buddy *ub, unsigned bits, struct ut_block **out);

/* Revokes a block (destroying anything made from it) and returns it to the free pool. */
LIBOS_MUST_CHECK int ut_buddy_free(struct ut_buddy *ub, struct ut_block *b);

#endif /* LIBOS_UT_BUDDY_H */
