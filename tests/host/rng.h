/* rng.h - small deterministic PRNG (splitmix64) for randomized host tests. */
#ifndef TEST_RNG_H
#define TEST_RNG_H

#include <stdint.h>

static inline uint64_t rng_next(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Uniform-ish value in [lo, hi]. */
static inline uint64_t rng_range(uint64_t *state, uint64_t lo, uint64_t hi)
{
    return lo + rng_next(state) % (hi - lo + 1);
}

#endif /* TEST_RNG_H */
