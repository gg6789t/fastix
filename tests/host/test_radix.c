/*
 * test_radix.c - randomized model test for libos/radix.
 * Runs 200 seeds mixing dense low indices, sparse medium indices, and full
 * 64-bit high indices (including UINT64_MAX), checking insert/update/get/del/
 * next/clear and OOM rollback against a reference array model with 0 node leaks.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fastix/config.h>
#include <fastix/errno.h>
#include <libos/radix.h>

#include "rng.h"

#define SEEDS     200
#define OPS       2500
#define KEY_SPACE 256
#define MAX_PAGES 512

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

struct test_provider {
    void *pages[MAX_PAGES];
    int used;
    int budget;
};

static int prov_get_page(void *ctx, void **page)
{
    struct test_provider *p = ctx;
    if (p->used >= p->budget) {
        return -FASTIX_ENOMEM;
    }
    void *mem = aligned_alloc(FASTIX_PAGE_SIZE, FASTIX_PAGE_SIZE);
    CHECK(mem != NULL);
    memset(mem, 0, FASTIX_PAGE_SIZE);
    p->pages[p->used++] = mem;
    *page = mem;
    return 0;
}

static void prov_free_all(struct test_provider *p)
{
    for (int i = 0; i < p->used; i++) {
        free(p->pages[i]);
    }
    p->used = 0;
}

static uint64_t keys[KEY_SPACE];
static void *model[KEY_SPACE];

static void *model_next(uint64_t start, uint64_t *found_idx)
{
    int best = -1;
    for (int i = 0; i < KEY_SPACE; i++) {
        if (model[i] != NULL && keys[i] >= start) {
            if (best < 0 || keys[i] < keys[best]) {
                best = i;
            }
        }
    }
    if (best < 0) {
        return NULL;
    }
    if (found_idx != NULL) {
        *found_idx = keys[best];
    }
    return model[best];
}

static void check_edges_and_oom(void)
{
    struct test_provider tp = { .used = 0, .budget = 1 };
    struct radix_tree t;
    struct slab_cache nodes;
    slab_init(&nodes, "radix_node", radix_node_size(), (struct slab_provider){ prov_get_page, &tp });
    radix_init(&t, &nodes);

    int v1 = 1, v2 = 2;
    CHECK(radix_set(&t, 0, NULL, NULL) == -FASTIX_EINVAL);
    CHECK(radix_get(&t, 0) == NULL);
    CHECK(radix_next(&t, 0, NULL) == NULL);

    /* Insert at 0 (height 1, 1 node). */
    CHECK(radix_set(&t, 0, &v1, NULL) == 0);
    CHECK(t.height == 1 && t.count == 1 && t.node_cache->in_use == 1);

    /* Insert at UINT64_MAX (needs height 16 -> 31 nodes total; 1 slab page fits ~28).
     * Must fail with ENOMEM and roll back cleanly without leaking nodes or losing key 0! */
    tp.budget = 1;
    int err = radix_set(&t, UINT64_MAX, &v2, NULL);
    CHECK(err == -FASTIX_ENOMEM);
    CHECK(t.height == 1 && t.count == 1 && t.node_cache->in_use == 1);
    CHECK(radix_get(&t, 0) == &v1);
    CHECK(radix_get(&t, UINT64_MAX) == NULL);

    /* Now allow enough pages and verify UINT64_MAX works and next handles UINT64_MAX. */
    tp.budget = 16;
    CHECK(radix_set(&t, UINT64_MAX, &v2, NULL) == 0);
    CHECK(t.height == RADIX_MAX_HEIGHT && t.count == 2);
    uint64_t fi = 0;
    CHECK(radix_next(&t, 0, &fi) == &v1 && fi == 0);
    CHECK(radix_next(&t, 1, &fi) == &v2 && fi == UINT64_MAX);
    CHECK(radix_next(&t, UINT64_MAX, &fi) == &v2 && fi == UINT64_MAX);

    /* Delete UINT64_MAX: root must shrink all the way back to height 1! */
    CHECK(radix_del(&t, UINT64_MAX) == &v2);
    CHECK(t.height == 1 && t.count == 1 && t.node_cache->in_use == 1);
    CHECK(radix_get(&t, 0) == &v1);

    /* Delete 0: 0 nodes in use. */
    CHECK(radix_del(&t, 0) == &v1);
    CHECK(t.height == 0 && t.root == NULL && t.count == 0 && t.node_cache->in_use == 0);
    prov_free_all(&tp);
}

static void run_seed(void)
{
    uint64_t rng = seed;
    struct test_provider tp = { .used = 0, .budget = (int)rng_range(&rng, 4, 64) };
    struct radix_tree t;
    struct slab_cache nodes;
    slab_init(&nodes, "radix_node", radix_node_size(), (struct slab_provider){ prov_get_page, &tp });
    radix_init(&t, &nodes);

    /* Mix dense [0..63], medium [0..65535], and huge 64-bit keys including UINT64_MAX. */
    for (int i = 0; i < KEY_SPACE; i++) {
        model[i] = NULL;
        if (i < 64) {
            keys[i] = (uint64_t)i;
        } else if (i < 160) {
            keys[i] = rng_range(&rng, 0, 65535);
        } else if (i == KEY_SPACE - 1) {
            keys[i] = UINT64_MAX;
        } else {
            keys[i] = rng_next(&rng);
        }
        for (int j = 0; j < i; j++) {
            if (keys[j] == keys[i]) {
                keys[i] ^= (uint64_t)(i + 1) * 0x10001ULL;
                j = -1;
            }
        }
    }

    uint64_t live = 0;
    for (op = 0; op < OPS; op++) {
        int kind = (int)rng_range(&rng, 0, 99);
        int idx = (int)rng_range(&rng, 0, KEY_SPACE - 1);

        if (kind < 45) {
            void *val = (void *)(uintptr_t)(rng_next(&rng) | 1ULL);
            void *old = (void *)0xdead;
            uint64_t nodes_before = t.node_cache->in_use;
            int err = radix_set(&t, keys[idx], val, &old);
            if (err == -FASTIX_ENOMEM) {
                /* Must have rolled back cleanly without leaking nodes or changing key. */
                CHECK(t.node_cache->in_use == nodes_before);
                CHECK(radix_get(&t, keys[idx]) == model[idx]);
            } else {
                CHECK(err == 0);
                CHECK(old == model[idx]);
                if (model[idx] == NULL) {
                    live++;
                }
                model[idx] = val;
            }
        } else if (kind < 75) {
            void *got = radix_del(&t, keys[idx]);
            CHECK(got == model[idx]);
            if (model[idx] != NULL) {
                model[idx] = NULL;
                live--;
            }
        } else if (kind < 90) {
            CHECK(radix_get(&t, keys[idx]) == model[idx]);
        } else {
            uint64_t start = (rng_range(&rng, 0, 1) == 0) ? keys[idx] : rng_next(&rng);
            uint64_t m_idx = 0, r_idx = 0;
            void *m_val = model_next(start, &m_idx);
            void *r_val = radix_next(&t, start, &r_idx);
            CHECK(r_val == m_val);
            if (m_val != NULL) {
                CHECK(r_idx == m_idx);
            }
        }
        CHECK(t.count == live);
        if (live == 0) {
            CHECK(t.height == 0 && t.root == NULL && t.node_cache->in_use == 0);
        }
    }

    /* Full ordered walk matches model. */
    uint64_t walked = 0;
    uint64_t cur = 0;
    for (;;) {
        uint64_t m_idx = 0, r_idx = 0;
        void *m_val = model_next(cur, &m_idx);
        void *r_val = radix_next(&t, cur, &r_idx);
        CHECK(r_val == m_val);
        if (r_val == NULL) {
            break;
        }
        CHECK(r_idx == m_idx);
        walked++;
        if (r_idx == UINT64_MAX) {
            break;
        }
        cur = r_idx + 1;
    }
    CHECK(walked == live);

    /* Alternate between radix_clear and individual deletes to verify 0 leaks on both. */
    if ((seed & 1) == 0) {
        radix_clear(&t);
    } else {
        for (int i = 0; i < KEY_SPACE; i++) {
            CHECK(radix_del(&t, keys[i]) == model[i]);
        }
    }
    CHECK(t.count == 0 && t.height == 0 && t.root == NULL && t.node_cache->in_use == 0);
    prov_free_all(&tp);
}

int main(void)
{
    check_edges_and_oom();
    for (seed = 1; seed <= SEEDS; seed++) {
        run_seed();
    }
    printf("test_radix: %d seeds x %d ops passed\n", SEEDS, OPS);
    return 0;
}
