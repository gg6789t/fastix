/*
 * test_hashtab.c - randomized model test for libos/hashtab.
 * Runs 200 seeds of insert/update/lookup/delete/grow/shrink/iterate against
 * a simple array model, plus edge-case checks (full table, NULL value, invalid cap).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <fastix/errno.h>
#include <libos/hashtab.h>

#include "rng.h"

#define SEEDS     200
#define OPS       2500
#define KEY_SPACE 300

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

static void *model[KEY_SPACE];
static uint64_t keys[KEY_SPACE];

static void check_edges(void)
{
    struct hashtab ht;
    struct hashtab_slot buf[4];
    int dummy = 1;

    CHECK(hashtab_init(&ht, buf, 0) == -FASTIX_EINVAL);
    CHECK(hashtab_init(&ht, buf, 1) == -FASTIX_EINVAL);
    CHECK(hashtab_init(&ht, buf, 3) == -FASTIX_EINVAL);
    CHECK(hashtab_init(&ht, buf, 4) == 0);
    CHECK(hashtab_set(&ht, 1, NULL, NULL) == -FASTIX_EINVAL);

    /* Capacity 4 holds at most 3 entries (one empty slot kept as sentinel). */
    CHECK(hashtab_set(&ht, 10, &dummy, NULL) == 0);
    CHECK(hashtab_set(&ht, 20, &dummy, NULL) == 0);
    CHECK(hashtab_set(&ht, 30, &dummy, NULL) == 0);
    CHECK(hashtab_should_grow(&ht));
    CHECK(hashtab_set(&ht, 40, &dummy, NULL) == -FASTIX_ENOSPC);
    /* Updating an existing key still works when full. */
    void *old = NULL;
    CHECK(hashtab_set(&ht, 20, (void *)0x1234, &old) == 0);
    CHECK(old == &dummy);
    CHECK(hashtab_get(&ht, 20) == (void *)0x1234);

    /* Rehash to too-small table fails without corrupting ht. */
    struct hashtab_slot small[2];
    CHECK(hashtab_rehash(&ht, small, 2) == -FASTIX_EINVAL);
    CHECK(ht.cap == 4 && ht.count == 3);
    CHECK(hashtab_get(&ht, 10) == &dummy);
    CHECK(hashtab_get(&ht, 30) == &dummy);
}

static void check_iter(const struct hashtab *ht, size_t expected_live)
{
    bool seen[KEY_SPACE] = { false };
    size_t cursor = 0;
    size_t seen_count = 0;
    const struct hashtab_slot *s;

    while ((s = hashtab_next(ht, &cursor)) != NULL) {
        int found = -1;
        for (int i = 0; i < KEY_SPACE; i++) {
            if (keys[i] == s->key) {
                found = i;
                break;
            }
        }
        CHECK(found >= 0 && !seen[found]);
        CHECK(model[found] == s->val);
        seen[found] = true;
        seen_count++;
    }
    CHECK(seen_count == expected_live);
}

static void run_seed(void)
{
    uint64_t rng = seed;
    size_t cap = 8;
    struct hashtab_slot *slots = calloc(cap, sizeof(*slots));
    CHECK(slots != NULL);
    struct hashtab ht;
    CHECK(hashtab_init(&ht, slots, cap) == 0);

    /* Mix small sequential keys and random 64-bit keys to test both patterns. */
    for (int i = 0; i < KEY_SPACE; i++) {
        model[i] = NULL;
        keys[i] = (i < 64) ? (uint64_t)i : rng_next(&rng);
        /* Ensure unique keys in this seed. */
        for (int j = 0; j < i; j++) {
            if (keys[j] == keys[i]) {
                keys[i] ^= (uint64_t)(i + 1) << 40;
                j = -1;
            }
        }
    }

    size_t live = 0;
    for (op = 0; op < OPS; op++) {
        int kind = (int)rng_range(&rng, 0, 99);
        int idx = (int)rng_range(&rng, 0, KEY_SPACE - 1);

        if (kind < 45) {
            if (hashtab_should_grow(&ht)) {
                size_t new_cap = ht.cap * 2;
                struct hashtab_slot *ns = calloc(new_cap, sizeof(*ns));
                CHECK(ns != NULL);
                CHECK(hashtab_rehash(&ht, ns, new_cap) == 0);
                free(slots);
                slots = ns;
            }
            void *val = (void *)(uintptr_t)(rng_next(&rng) | 1ULL);
            void *old = (void *)0xdead;
            int err = hashtab_set(&ht, keys[idx], val, &old);
            CHECK(err == 0);
            CHECK(old == model[idx]);
            if (model[idx] == NULL) {
                live++;
            }
            model[idx] = val;
        } else if (kind < 75) {
            void *got = hashtab_del(&ht, keys[idx]);
            CHECK(got == model[idx]);
            if (model[idx] != NULL) {
                model[idx] = NULL;
                live--;
            }
            /* Occasionally shrink when sparse to test shrinking rehash too. */
            if (ht.cap > 8 && ht.count * 8 < ht.cap && rng_range(&rng, 0, 7) == 0) {
                size_t new_cap = ht.cap / 2;
                struct hashtab_slot *ns = calloc(new_cap, sizeof(*ns));
                CHECK(ns != NULL);
                CHECK(hashtab_rehash(&ht, ns, new_cap) == 0);
                free(slots);
                slots = ns;
            }
        } else {
            CHECK(hashtab_get(&ht, keys[idx]) == model[idx]);
        }

        CHECK(ht.count == live);
        if ((op % 250) == 0) {
            for (int i = 0; i < KEY_SPACE; i++) {
                CHECK(hashtab_get(&ht, keys[i]) == model[i]);
            }
            check_iter(&ht, live);
        }
    }

    check_iter(&ht, live);
    /* Delete everything and confirm clean empty state. */
    for (int i = 0; i < KEY_SPACE; i++) {
        void *got = hashtab_del(&ht, keys[i]);
        CHECK(got == model[i]);
        model[i] = NULL;
    }
    CHECK(ht.count == 0);
    check_iter(&ht, 0);
    free(slots);
}

int main(void)
{
    check_edges();
    for (seed = 1; seed <= SEEDS; seed++) {
        run_seed();
    }
    printf("test_hashtab: %d seeds x %d ops passed\n", SEEDS, OPS);
    return 0;
}
