/*
 * libos/rtree.h - balanced (AVL) tree of non-overlapping address ranges.
 * Used for memory areas (VMAs) and mapped pages. Embed a struct rnode in
 * your struct. A node may be shrunk in place (start raised or end lowered)
 * without removing it, because that never changes the order of the ranges.
 */
#ifndef LIBOS_RTREE_H
#define LIBOS_RTREE_H

#include <stdint.h>

#include <libos/compiler.h>

struct rnode {
    struct rnode *left;
    struct rnode *right;
    uint64_t start; /* range is [start, end) */
    uint64_t end;
    int height;
};

struct rtree {
    struct rnode *root;
    uint64_t count;
};

void rtree_init(struct rtree *t);

/* Inserts n (n->start < n->end). Returns 0, -FASTIX_EINVAL, or -FASTIX_EEXIST on overlap. */
LIBOS_MUST_CHECK int rtree_insert(struct rtree *t, struct rnode *n);

/* Removes n, which must be in the tree. */
void rtree_remove(struct rtree *t, struct rnode *n);

/* The node whose range contains addr, or NULL. */
struct rnode *rtree_find(const struct rtree *t, uint64_t addr);

/* The lowest node that ends after addr (contains addr or lies above it), or NULL.
 * Walk all nodes in order with: for (n = rtree_next(t, 0); n; n = rtree_next(t, n->end)) */
struct rnode *rtree_next(const struct rtree *t, uint64_t addr);

#endif /* LIBOS_RTREE_H */
