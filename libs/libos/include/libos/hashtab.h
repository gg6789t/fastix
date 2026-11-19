/*
 * libos/hashtab.h - open-addressed hash table mapping uint64_t -> non-NULL void *.
 * Uses linear probing with backward-shift deletion (no tombstones).
 * The caller owns the backing slot array; grow by allocating a larger power-of-2
 * array and calling hashtab_rehash().
 */
#ifndef LIBOS_HASHTAB_H
#define LIBOS_HASHTAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libos/compiler.h>

struct hashtab_slot {
    uint64_t key;
    void *val; /* NULL means empty slot */
};

struct hashtab {
    struct hashtab_slot *slots;
    size_t cap;   /* power of 2, >= 2 */
    size_t count; /* occupied slots (always < cap) */
};

/* Initializes ht over caller-supplied slots[0..cap-1] (zeroes slots).
 * cap must be a power of 2 >= 2. Returns 0 or -FASTIX_EINVAL. */
LIBOS_MUST_CHECK int hashtab_init(struct hashtab *ht, struct hashtab_slot *slots, size_t cap);

/* Returns the value for key, or NULL if absent. */
void *hashtab_get(const struct hashtab *ht, uint64_t key);

/* Inserts or updates key -> val (val must be non-NULL).
 * If old_out != NULL, writes the previous value (or NULL if newly inserted).
 * Returns 0, -FASTIX_EINVAL, or -FASTIX_ENOSPC (when count == cap - 1). */
LIBOS_MUST_CHECK int hashtab_set(struct hashtab *ht, uint64_t key, void *val, void **old_out);

/* Removes key if present and returns its value, or NULL if absent. */
void *hashtab_del(struct hashtab *ht, uint64_t key);

/* True when load >= 75% (caller should allocate a larger array and rehash). */
static inline bool hashtab_should_grow(const struct hashtab *ht)
{
    return ht->count * 4 >= ht->cap * 3;
}

/* Rehashes all entries into new_slots[0..new_cap-1].
 * new_cap must be a power of 2 >= 2 and strictly greater than ht->count.
 * On error, ht is left unchanged. Returns 0 or -FASTIX_EINVAL. */
LIBOS_MUST_CHECK int hashtab_rehash(struct hashtab *ht, struct hashtab_slot *new_slots,
                                    size_t new_cap);

/* Walks occupied slots. Start with *cursor = 0; returns the next slot or NULL. */
const struct hashtab_slot *hashtab_next(const struct hashtab *ht, size_t *cursor);

#endif /* LIBOS_HASHTAB_H */
