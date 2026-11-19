/*
 * test_ut_buddy.c - randomized model test for libos/ut_buddy.
 *
 * A fake backend models seL4 untyped caps: each retype carves the next
 * aligned piece from the parent's watermark, and revoke deletes every
 * descendant cap and resets the watermark. After every operation the test
 * checks the allocator's view against that model; at the end it frees
 * everything and requires a fully merged, leak-free state.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <fastix/errno.h>
#include <libos/ut_buddy.h>

#include "rng.h"

#define SEEDS        200
#define OPS          3000
#define MAX_ROOTS    6
#define ROOT_SLOTS   16                  /* slots [0, 16) hold root untypeds */
#define MANAGED      4000                /* slots [16, 16 + MANAGED) for children */
#define TOTAL_SLOTS  (ROOT_SLOTS + MANAGED)
#define MAX_NODES    600
#define NO_PARENT    UINT64_MAX
#define INJECTED_ERR (-FASTIX_EIO)       /* distinguishes injected failures from real ENOMEM */

struct model_cap {
    bool live;
    uint64_t parent;
    uint64_t paddr;
    unsigned bits;
    uint64_t watermark;
};

static struct model_cap caps[TOTAL_SLOTS];
static uint64_t rng;
static unsigned split_fail_percent;
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

static int model_split(void *ctx, uint64_t parent, uint64_t slot, unsigned bits)
{
    (void)ctx;
    CHECK(parent < TOTAL_SLOTS && caps[parent].live);
    CHECK(slot >= ROOT_SLOTS && slot < TOTAL_SLOTS && !caps[slot].live);
    CHECK(bits >= UT_MIN_BITS && bits < caps[parent].bits);
    if (rng_range(&rng, 0, 99) < split_fail_percent) {
        return INJECTED_ERR;
    }
    uint64_t size = 1ULL << bits;
    uint64_t offset = (caps[parent].watermark + size - 1) & ~(size - 1);
    CHECK(offset + size <= (1ULL << caps[parent].bits));
    caps[slot] = (struct model_cap){ true, parent, caps[parent].paddr + offset, bits, 0 };
    caps[parent].watermark = offset + size;
    return 0;
}

static bool descends_from(uint64_t slot, uint64_t ancestor)
{
    for (uint64_t p = caps[slot].parent; p != NO_PARENT; p = caps[p].parent) {
        if (p == ancestor) {
            return true;
        }
    }
    return false;
}

static int model_revoke(void *ctx, uint64_t parent)
{
    (void)ctx;
    bool doomed[TOTAL_SLOTS];

    CHECK(parent < TOTAL_SLOTS && caps[parent].live);
    for (uint64_t s = 0; s < TOTAL_SLOTS; s++) {
        doomed[s] = caps[s].live && descends_from(s, parent);
    }
    for (uint64_t s = 0; s < TOTAL_SLOTS; s++) {
        if (doomed[s]) {
            caps[s].live = false;
        }
    }
    caps[parent].watermark = 0;
    return 0;
}

static void check_block(const struct ut_block *b)
{
    CHECK(caps[b->slot].live);
    CHECK(caps[b->slot].paddr == b->paddr);
    CHECK(caps[b->slot].bits == b->bits);
}

static int cmp_block(const void *a, const void *b)
{
    const struct ut_block *x = *(const struct ut_block *const *)a;
    const struct ut_block *y = *(const struct ut_block *const *)b;
    return (x->paddr > y->paddr) - (x->paddr < y->paddr);
}

static void check_invariants(struct ut_buddy *ub, struct ut_block **used, int n_used,
                             uint64_t total_bytes)
{
    static const struct ut_block *all[MAX_NODES];
    int n_all = 0;
    uint64_t free_bytes = 0, used_bytes = 0;

    for (int k = 0; k <= UT_MAX_BITS; k++) {
        for (struct list_node *n = ub->free_list[k].next; n != &ub->free_list[k];
             n = n->next) {
            const struct ut_block *b = LIST_ENTRY(n, struct ut_block, link);
            CHECK(b->state == UT_FREE && b->bits == k);
            check_block(b);
            /* Fully merged: a free block's buddy is never also free. */
            if (b->parent != NULL) {
                const struct ut_block *buddy =
                    (b->parent->child[0] == b) ? b->parent->child[1] : b->parent->child[0];
                CHECK(buddy->state != UT_FREE);
            }
            free_bytes += 1ULL << b->bits;
            all[n_all++] = b;
        }
    }
    for (int i = 0; i < n_used; i++) {
        CHECK(used[i]->state == UT_USED);
        check_block(used[i]);
        used_bytes += 1ULL << used[i]->bits;
        all[n_all++] = used[i];
    }
    CHECK(free_bytes == ub->free_bytes);
    CHECK(free_bytes + used_bytes == total_bytes); /* no memory lost */

    /* Free and used blocks never overlap. */
    qsort(all, (size_t)n_all, sizeof(all[0]), cmp_block);
    for (int i = 1; i < n_all; i++) {
        CHECK(all[i - 1]->paddr + (1ULL << all[i - 1]->bits) <= all[i]->paddr);
    }

    /* Every child untyped cap in the model is accounted for by a slot. */
    uint64_t live_children = 0;
    for (uint64_t s = ROOT_SLOTS; s < TOTAL_SLOTS; s++) {
        live_children += caps[s].live;
    }
    CHECK(live_children == ub->slots->used);
}

static void run_seed(void)
{
    static struct ut_block nodes[MAX_NODES];
    static uint64_t bitmap[SLOT_ALLOC_WORDS(MANAGED)];
    static struct ut_block *used[MAX_NODES];
    struct slot_alloc slots;
    struct ut_buddy ub;
    int n_used = 0, n_roots = (int)rng_range(&rng, 1, MAX_ROOTS);
    uint64_t total_bytes = 0, paddr = 0x40000000;

    for (uint64_t s = 0; s < TOTAL_SLOTS; s++) {
        caps[s].live = false;
    }
    split_fail_percent = (unsigned)rng_range(&rng, 0, 3);
    slot_alloc_init(&slots, ROOT_SLOTS, MANAGED, bitmap);
    ut_buddy_init(&ub, nodes, MAX_NODES, &slots,
                  (struct ut_backend){ model_split, model_revoke, NULL });

    op = -1;
    for (int r = 0; r < n_roots; r++) {
        unsigned bits = (unsigned)rng_range(&rng, 10, 22);
        caps[r] = (struct model_cap){ true, NO_PARENT, paddr, bits, 0 };
        CHECK(ut_buddy_add(&ub, (uint64_t)r, paddr, bits) == 0);
        paddr += 1ULL << bits;
        total_bytes += 1ULL << bits;
    }

    for (op = 0; op < OPS; op++) {
        if (n_used == 0 || rng_range(&rng, 0, 99) < 60) {
            unsigned bits = (unsigned)rng_range(&rng, UT_MIN_BITS, 20);
            struct ut_block *b = NULL;
            int err = ut_buddy_alloc(&ub, bits, &b);
            if (err == 0) {
                CHECK(b->bits == bits);
                used[n_used++] = b;
            } else if (err == -FASTIX_ENOMEM) {
                /* Legitimate only if no free block is big enough, or splitting the
                 * smallest big-enough block needs more nodes/slots than are left
                 * (each split takes 2 of each). */
                unsigned k = bits;
                while (k <= UT_MAX_BITS && list_empty(&ub.free_list[k])) {
                    k++;
                }
                uint64_t need = 2ULL * (k - bits);
                CHECK(k > UT_MAX_BITS || ub.blocks_in_use + need > MAX_NODES ||
                      ub.slots->used + need > MANAGED);
            } else {
                CHECK(err == INJECTED_ERR);
            }
        } else {
            int i = (int)rng_range(&rng, 0, (uint64_t)n_used - 1);
            CHECK(ut_buddy_free(&ub, used[i]) == 0);
            used[i] = used[--n_used];
        }
        check_invariants(&ub, used, n_used, total_bytes);
    }

    /* Free everything: must end fully merged with zero leaks. */
    while (n_used > 0) {
        CHECK(ut_buddy_free(&ub, used[--n_used]) == 0);
    }
    check_invariants(&ub, used, 0, total_bytes);
    CHECK(ub.free_bytes == total_bytes);
    CHECK(ub.blocks_in_use == (uint64_t)n_roots);
    CHECK(ub.slots->used == 0);
    for (int r = 0; r < n_roots; r++) {
        CHECK(caps[r].live && caps[r].watermark == 0);
    }
}

int main(void)
{
    for (seed = 1; seed <= SEEDS; seed++) {
        rng = seed;
        run_seed();
    }
    printf("test_ut_buddy: %d seeds x %d ops passed (with injected split failures)\n", SEEDS, OPS);
    return 0;
}
