/*
 * test_slot_alloc.c - randomized model test for libos/slot_alloc.
 * Each seed picks a slot range and runs random get/put operations, checking
 * every result against a plain boolean array.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <fastix/errno.h>
#include <libos/slot_alloc.h>

#include "rng.h"

#define SEEDS     200
#define OPS       5000
#define MAX_COUNT 300

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL seed %llu op %d: %s (line %d)\n", \
                    (unsigned long long)seed, op, #cond, __LINE__); \
            exit(1); \
        } \
    } while (0)

static void run_seed(uint64_t seed)
{
    uint64_t rng = seed;
    uint64_t count = rng_range(&rng, 1, MAX_COUNT);
    uint64_t first = rng_range(&rng, 0, 5000);
    uint64_t bits[SLOT_ALLOC_WORDS(MAX_COUNT)];
    bool model[MAX_COUNT] = { false };
    uint64_t used = 0;
    struct slot_alloc sa;
    int op = -1;

    slot_alloc_init(&sa, first, count, bits);
    for (op = 0; op < OPS; op++) {
        if (rng_range(&rng, 0, 99) < 55) {
            uint64_t slot = 0;
            int err = slot_alloc_get(&sa, &slot);
            if (used == count) {
                CHECK(err == -FASTIX_ENOMEM);
                continue;
            }
            CHECK(err == 0);
            CHECK(slot >= first && slot < first + count);
            CHECK(!model[slot - first]);
            model[slot - first] = true;
            used++;
        } else if (used > 0) {
            uint64_t pick = rng_range(&rng, 0, count - 1);
            while (!model[pick]) {
                pick = (pick + 1) % count;
            }
            slot_alloc_put(&sa, first + pick);
            model[pick] = false;
            used--;
        }
        CHECK(sa.used == used);
    }
    /* Return everything; every slot must then be obtainable again. */
    for (uint64_t i = 0; i < count; i++) {
        if (model[i]) {
            slot_alloc_put(&sa, first + i);
        }
    }
    CHECK(sa.used == 0);
    for (uint64_t i = 0; i < count; i++) {
        uint64_t slot;
        CHECK(slot_alloc_get(&sa, &slot) == 0);
    }
    uint64_t extra;
    CHECK(slot_alloc_get(&sa, &extra) == -FASTIX_ENOMEM);
}

int main(void)
{
    for (uint64_t seed = 1; seed <= SEEDS; seed++) {
        run_seed(seed);
    }
    printf("test_slot_alloc: %d seeds x %d ops passed\n", SEEDS, OPS);
    return 0;
}
