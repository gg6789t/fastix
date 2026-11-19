/*
 * test_slab.c - randomized model test for libos/slab.
 * Several caches of random object sizes hand out objects; each live object
 * is filled with a pattern unique to it, and the pattern must be intact when
 * it is freed (catches overlapping or double-handed-out objects). A provider
 * with a page budget tests the out-of-memory path.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fastix/config.h>
#include <fastix/errno.h>
#include <libos/slab.h>

#include "rng.h"

#define SEEDS   200
#define OPS     3000
#define CACHES  3
#define MAXLIVE 2000
#define MAXPAGE 256

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

struct provider_state {
    void *pages[MAXPAGE];
    int used;
    int budget;
};

static int get_page(void *ctx, void **page)
{
    struct provider_state *ps = ctx;
    if (ps->used == ps->budget) {
        return -FASTIX_ENOMEM;
    }
    void *p = aligned_alloc(FASTIX_PAGE_SIZE, FASTIX_PAGE_SIZE);
    CHECK(p != NULL);
    memset(p, 0, FASTIX_PAGE_SIZE);
    ps->pages[ps->used++] = p;
    *page = p;
    return 0;
}

struct live_obj {
    uint8_t *p;
    int cache;
    uint8_t tag;
};

static bool in_provider_page(const struct provider_state *ps, const uint8_t *p, size_t size)
{
    for (int i = 0; i < ps->used; i++) {
        const uint8_t *base = ps->pages[i];
        if (p >= base && p + size <= base + FASTIX_PAGE_SIZE) {
            return true;
        }
    }
    return false;
}

static void run_seed(void)
{
    uint64_t rng = seed;
    struct provider_state ps = { .used = 0, .budget = (int)rng_range(&rng, 4, MAXPAGE) };
    struct slab_cache caches[CACHES];
    static struct live_obj live[MAXLIVE];
    int n_live = 0;
    uint64_t in_use[CACHES] = { 0 };

    for (int c = 0; c < CACHES; c++) {
        size_t size = (size_t)rng_range(&rng, 1, rng_range(&rng, 0, 9) == 0 ? FASTIX_PAGE_SIZE : 300);
        slab_init(&caches[c], "test", size, (struct slab_provider){ get_page, &ps });
        CHECK(caches[c].obj_size % SLAB_ALIGN == 0 && caches[c].obj_size >= size);
    }
    for (op = 0; op < OPS; op++) {
        int c = (int)rng_range(&rng, 0, CACHES - 1);
        if (n_live < MAXLIVE && (n_live == 0 || rng_range(&rng, 0, 99) < 55)) {
            int pages_before = ps.used;
            uint8_t *p = slab_alloc(&caches[c]);
            if (p == NULL) {
                CHECK(ps.used == ps.budget && pages_before == ps.used);
                continue;
            }
            size_t size = caches[c].obj_size;
            CHECK(((uintptr_t)p % SLAB_ALIGN) == 0 && in_provider_page(&ps, p, size));
            for (size_t i = 0; i < size; i++) {
                CHECK(p[i] == 0); /* handed out zeroed */
            }
            uint8_t tag = (uint8_t)rng_next(&rng);
            memset(p, tag, size);
            live[n_live++] = (struct live_obj){ p, c, tag };
            in_use[c]++;
        } else {
            int i = (int)rng_range(&rng, 0, (uint64_t)n_live - 1);
            struct live_obj o = live[i];
            for (size_t k = 0; k < caches[o.cache].obj_size; k++) {
                CHECK(o.p[k] == o.tag); /* nobody else wrote into it */
            }
            slab_free(&caches[o.cache], o.p);
            in_use[o.cache]--;
            live[i] = live[--n_live];
        }
        for (int k = 0; k < CACHES; k++) {
            CHECK(caches[k].in_use == in_use[k]);
        }
    }
    while (n_live > 0) {
        struct live_obj o = live[--n_live];
        slab_free(&caches[o.cache], o.p);
    }
    uint64_t pages = 0;
    for (int k = 0; k < CACHES; k++) {
        CHECK(caches[k].in_use == 0);
        pages += caches[k].pages;
    }
    CHECK(pages == (uint64_t)ps.used);
    for (int i = 0; i < ps.used; i++) {
        free(ps.pages[i]);
    }
}

int main(void)
{
    for (seed = 1; seed <= SEEDS; seed++) {
        run_seed();
    }
    printf("test_slab: %d seeds x %d ops passed\n", SEEDS, OPS);
    return 0;
}
