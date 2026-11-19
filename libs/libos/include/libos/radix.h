/*
 * libos/radix.h - sparse uint64_t -> non-NULL void * radix tree.
 * 16-way (4 bits/level) tree backed by a libos slab cache. Automatically grows
 * height for large indices and prunes empty internal nodes on delete (0 leaks).
 */
#ifndef LIBOS_RADIX_H
#define LIBOS_RADIX_H

#include <stddef.h>
#include <stdint.h>

#include <libos/compiler.h>
#include <libos/slab.h>

#define RADIX_BITS       4
#define RADIX_FANOUT     (1U << RADIX_BITS) /* 16 */
#define RADIX_MASK       (RADIX_FANOUT - 1U)
#define RADIX_MAX_HEIGHT (64U / RADIX_BITS) /* 16 */

struct radix_node;

struct radix_tree {
    struct radix_node *root;
    struct slab_cache node_cache;
    unsigned height; /* 0 when empty, 1..RADIX_MAX_HEIGHT otherwise */
    uint64_t count;  /* live entries */
};

/* Initializes an empty tree whose internal nodes come from provider. */
void radix_init(struct radix_tree *t, struct slab_provider provider);

/* Returns the value stored at index, or NULL if absent. */
void *radix_get(const struct radix_tree *t, uint64_t index);

/* Inserts or replaces index -> val (val must be non-NULL).
 * If old_out != NULL, writes the previous value (or NULL if newly inserted).
 * Returns 0, -FASTIX_EINVAL, or -FASTIX_ENOMEM (leaving the tree unchanged). */
LIBOS_MUST_CHECK int radix_set(struct radix_tree *t, uint64_t index, void *val, void **old_out);

/* Removes index if present and returns its value, or NULL if absent. */
void *radix_del(struct radix_tree *t, uint64_t index);

/* Finds the lowest-index entry with index >= start_index.
 * Returns its value (writing its key to *found_index if non-NULL), or NULL. */
void *radix_next(const struct radix_tree *t, uint64_t start_index, uint64_t *found_index);

/* Frees all internal nodes back to the tree's slab cache and resets to empty. */
void radix_clear(struct radix_tree *t);

#endif /* LIBOS_RADIX_H */
