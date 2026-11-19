/*
 * libs/libos/ds/hashtab.c - linear-probing hash table with backward-shift delete.
 */
#include <fastix/errno.h>
#include <libos/hashtab.h>
#include <libos/mem.h>

static bool is_pow2_ge2(size_t n)
{
    return n >= 2 && (n & (n - 1)) == 0;
}

/* SplitMix64 finalizer: spreads sequential and aligned keys evenly. */
static size_t hash_key(uint64_t k, size_t mask)
{
    k ^= k >> 30;
    k *= 0xbf58476d1ce4e5b9ULL;
    k ^= k >> 27;
    k *= 0x94d049bb133111ebULL;
    k ^= k >> 31;
    return (size_t)k & mask;
}

int hashtab_init(struct hashtab *ht, struct hashtab_slot *slots, size_t cap)
{
    if (ht == NULL || slots == NULL || !is_pow2_ge2(cap)) {
        return -FASTIX_EINVAL;
    }
    memset(slots, 0, cap * sizeof(*slots));
    ht->slots = slots;
    ht->cap = cap;
    ht->count = 0;
    return 0;
}

void *hashtab_get(const struct hashtab *ht, uint64_t key)
{
    if (ht == NULL || ht->slots == NULL || ht->count == 0) {
        return NULL;
    }
    size_t mask = ht->cap - 1;
    size_t idx = hash_key(key, mask);

    while (ht->slots[idx].val != NULL) {
        if (ht->slots[idx].key == key) {
            return ht->slots[idx].val;
        }
        idx = (idx + 1) & mask;
    }
    return NULL;
}

int hashtab_set(struct hashtab *ht, uint64_t key, void *val, void **old_out)
{
    if (ht == NULL || ht->slots == NULL || val == NULL) {
        return -FASTIX_EINVAL;
    }
    size_t mask = ht->cap - 1;
    size_t idx = hash_key(key, mask);

    while (ht->slots[idx].val != NULL) {
        if (ht->slots[idx].key == key) {
            if (old_out != NULL) {
                *old_out = ht->slots[idx].val;
            }
            ht->slots[idx].val = val;
            return 0;
        }
        idx = (idx + 1) & mask;
    }
    /* Keep at least one empty slot so probes always terminate. */
    if (ht->count + 1 >= ht->cap) {
        return -FASTIX_ENOSPC;
    }
    ht->slots[idx].key = key;
    ht->slots[idx].val = val;
    ht->count++;
    if (old_out != NULL) {
        *old_out = NULL;
    }
    return 0;
}

void *hashtab_del(struct hashtab *ht, uint64_t key)
{
    if (ht == NULL || ht->slots == NULL || ht->count == 0) {
        return NULL;
    }
    size_t mask = ht->cap - 1;
    size_t idx = hash_key(key, mask);

    while (ht->slots[idx].val != NULL) {
        if (ht->slots[idx].key == key) {
            void *removed = ht->slots[idx].val;
            size_t hole = idx;
            size_t cur = (idx + 1) & mask;

            /* Backward-shift subsequent entries whose home slot precedes or equals hole. */
            while (ht->slots[cur].val != NULL) {
                size_t home = hash_key(ht->slots[cur].key, mask);
                size_t dist_cur = (cur - home) & mask;
                size_t dist_hole = (cur - hole) & mask;

                if (dist_cur >= dist_hole) {
                    ht->slots[hole] = ht->slots[cur];
                    hole = cur;
                }
                cur = (cur + 1) & mask;
            }
            ht->slots[hole].key = 0;
            ht->slots[hole].val = NULL;
            ht->count--;
            return removed;
        }
        idx = (idx + 1) & mask;
    }
    return NULL;
}

int hashtab_rehash(struct hashtab *ht, struct hashtab_slot *new_slots, size_t new_cap)
{
    if (ht == NULL || ht->slots == NULL || new_slots == NULL || !is_pow2_ge2(new_cap) ||
        ht->count + 1 > new_cap) {
        return -FASTIX_EINVAL;
    }
    struct hashtab next;
    int err = hashtab_init(&next, new_slots, new_cap);
    if (err != 0) {
        return err;
    }
    for (size_t i = 0; i < ht->cap; i++) {
        if (ht->slots[i].val != NULL) {
            err = hashtab_set(&next, ht->slots[i].key, ht->slots[i].val, NULL);
            if (err != 0) {
                return err;
            }
        }
    }
    *ht = next;
    return 0;
}

const struct hashtab_slot *hashtab_next(const struct hashtab *ht, size_t *cursor)
{
    if (ht == NULL || ht->slots == NULL || cursor == NULL) {
        return NULL;
    }
    while (*cursor < ht->cap) {
        size_t i = (*cursor)++;
        if (ht->slots[i].val != NULL) {
            return &ht->slots[i];
        }
    }
    return NULL;
}
