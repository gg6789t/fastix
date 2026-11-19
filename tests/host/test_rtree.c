/*
 * test_rtree.c - randomized model test for libos/rtree.
 * Random inserts, removals, in-place shrinks and lookups are compared with a
 * plain array of ranges; the AVL shape (order, heights, balance) is checked
 * after every operation.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <fastix/errno.h>
#include <libos/rtree.h>

#include "rng.h"

#define SEEDS 200
#define OPS   3000
#define MAXN  400
#define SPACE 100000 /* addresses are drawn from [0, SPACE) */

static uint64_t seed;
static int op;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL seed %llu op %d: %s (line %d)\n", \
                    (unsigned long long)seed, op, #cond, __LINE__); \
            exit(1); \
        } \
    } while (0)

static struct rnode nodes[MAXN];
static bool in_tree[MAXN];

/* Returns the subtree height; checks order bounds, stored heights and balance. */
static int check_shape(const struct rnode *n, uint64_t lo, uint64_t hi, uint64_t *count)
{
    if (n == NULL) {
        return 0;
    }
    CHECK(n->start < n->end && n->start >= lo && n->end <= hi);
    int l = check_shape(n->left, lo, n->start, count);
    int r = check_shape(n->right, n->end, hi, count);
    CHECK(l - r <= 1 && r - l <= 1);
    CHECK(n->height == 1 + (l > r ? l : r));
    (*count)++;
    return n->height;
}

static const struct rnode *model_find(uint64_t addr)
{
    for (int i = 0; i < MAXN; i++) {
        if (in_tree[i] && nodes[i].start <= addr && addr < nodes[i].end) {
            return &nodes[i];
        }
    }
    return NULL;
}

static const struct rnode *model_next(uint64_t addr)
{
    const struct rnode *best = NULL;
    for (int i = 0; i < MAXN; i++) {
        if (in_tree[i] && nodes[i].end > addr && (best == NULL || nodes[i].start < best->start)) {
            best = &nodes[i];
        }
    }
    return best;
}

static bool model_overlaps(uint64_t start, uint64_t end)
{
    for (int i = 0; i < MAXN; i++) {
        if (in_tree[i] && nodes[i].start < end && start < nodes[i].end) {
            return true;
        }
    }
    return false;
}

static void run_seed(void)
{
    uint64_t rng = seed;
    struct rtree t;
    uint64_t live = 0;

    rtree_init(&t);
    for (int i = 0; i < MAXN; i++) {
        in_tree[i] = false;
    }
    for (op = 0; op < OPS; op++) {
        int kind = (int)rng_range(&rng, 0, 99);
        int i = (int)rng_range(&rng, 0, MAXN - 1);
        if (kind < 45 && !in_tree[i]) {
            uint64_t start = rng_range(&rng, 0, SPACE - 2);
            uint64_t end = start + rng_range(&rng, 1, 500);
            nodes[i].start = start;
            nodes[i].end = end > SPACE ? SPACE : end;
            bool clash = model_overlaps(nodes[i].start, nodes[i].end);
            int err = rtree_insert(&t, &nodes[i]);
            CHECK(clash ? err == -FASTIX_EEXIST : err == 0);
            if (err == 0) {
                in_tree[i] = true;
                live++;
            }
        } else if (kind < 75 && in_tree[i]) {
            rtree_remove(&t, &nodes[i]);
            in_tree[i] = false;
            live--;
        } else if (kind < 85 && in_tree[i] && nodes[i].end - nodes[i].start > 1) {
            /* Shrink in place from either side. */
            if (rng_range(&rng, 0, 1) == 0) {
                nodes[i].start++;
            } else {
                nodes[i].end--;
            }
        } else {
            uint64_t addr = rng_range(&rng, 0, SPACE);
            CHECK(rtree_find(&t, addr) == model_find(addr));
            CHECK(rtree_next(&t, addr) == model_next(addr));
        }
        uint64_t count = 0;
        check_shape(t.root, 0, SPACE, &count);
        CHECK(count == live && t.count == live);
    }
    /* In-order walk visits every node exactly once, in address order. */
    uint64_t walked = 0, last_end = 0;
    for (const struct rnode *n = rtree_next(&t, 0); n != NULL; n = rtree_next(&t, n->end)) {
        CHECK(n->start >= last_end);
        last_end = n->end;
        walked++;
    }
    CHECK(walked == live);
}

int main(void)
{
    for (seed = 1; seed <= SEEDS; seed++) {
        run_seed();
    }
    printf("test_rtree: %d seeds x %d ops passed\n", SEEDS, OPS);
    return 0;
}
