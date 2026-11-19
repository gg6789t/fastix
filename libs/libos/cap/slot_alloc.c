/* slot_alloc.c - CNode slot bitmap allocator (see libos/slot_alloc.h). */
#include <libos/assert.h>
#include <libos/slot_alloc.h>

#include <fastix/errno.h>

void slot_alloc_init(struct slot_alloc *sa, uint64_t first, uint64_t count, uint64_t *bits)
{
    uint64_t words = SLOT_ALLOC_WORDS(count);

    for (uint64_t w = 0; w < words; w++) {
        bits[w] = 0;
    }
    /* Mark the bits past `count` in the last word as used so they are never handed out. */
    if (count % 64 != 0) {
        bits[words - 1] = ~0ULL << (count % 64);
    }
    sa->first = first;
    sa->count = count;
    sa->used = 0;
    sa->hint = 0;
    sa->bits = bits;
}

int slot_alloc_get(struct slot_alloc *sa, uint64_t *slot)
{
    uint64_t words = SLOT_ALLOC_WORDS(sa->count);

    if (sa->used == sa->count) {
        return -FASTIX_ENOMEM;
    }
    for (uint64_t i = 0; i < words; i++) {
        uint64_t w = (sa->hint + i) % words;
        uint64_t free_bits = ~sa->bits[w];

        if (free_bits != 0) {
            uint64_t bit = (uint64_t)__builtin_ctzll(free_bits);
            sa->bits[w] |= 1ULL << bit;
            sa->used++;
            sa->hint = w;
            *slot = sa->first + w * 64 + bit;
            return 0;
        }
    }
    libos_assert(0 && "used count says a slot is free but the bitmap has none");
    return -FASTIX_ENOMEM;
}

void slot_alloc_put(struct slot_alloc *sa, uint64_t slot)
{
    libos_assert(slot >= sa->first && slot - sa->first < sa->count);
    uint64_t index = slot - sa->first;
    uint64_t mask = 1ULL << (index % 64);

    libos_assert((sa->bits[index / 64] & mask) != 0); /* double free */
    sa->bits[index / 64] &= ~mask;
    sa->used--;
}
