/*
 * test_vdso.c - checks the VDSO clock math in fastix/vdso.h against exact
 * 128-bit arithmetic for random counter frequencies and elapsed times, and
 * checks that time never goes backwards between consecutive counter values.
 */
#include <stdio.h>
#include <stdlib.h>

#include <fastix/vdso.h>

#include "rng.h"

#define SEEDS   200
#define SAMPLES 5000

__extension__ typedef unsigned __int128 u128;

int main(void)
{
    for (uint64_t seed = 1; seed <= SEEDS; seed++) {
        uint64_t rng = seed;
        uint64_t freq = rng_range(&rng, 1000000, 4000000000ULL); /* 1 MHz .. 4 GHz */
        struct fastix_vdso_data d = {
            .shift = FASTIX_VDSO_SHIFT,
            .mult = fastix_vdso_mult(freq),
            .counter_base = rng_next(&rng),
            .mono_base_ns = rng_range(&rng, 0, 1ULL << 50),
        };
        for (int i = 0; i < SAMPLES; i++) {
            uint64_t delta = rng_next(&rng) >> rng_range(&rng, 14, 63); /* up to 2^50 ticks */
            uint64_t counter = d.counter_base + delta;
            uint64_t got = fastix_vdso_mono_ns(&d, counter) - d.mono_base_ns;
            uint64_t exact = (uint64_t)((u128)delta * 1000000000ULL / freq);
            /* Rounding mult to an integer costs < 1 ns per 2^32 ns of elapsed time. */
            uint64_t slack = 1 + exact / (1ULL << 30);
            uint64_t err = got > exact ? got - exact : exact - got;
            if (err > slack) {
                fprintf(stderr, "FAIL seed %llu: freq %llu delta %llu got %llu exact %llu\n",
                        (unsigned long long)seed, (unsigned long long)freq,
                        (unsigned long long)delta, (unsigned long long)got,
                        (unsigned long long)exact);
                return 1;
            }
            if (fastix_vdso_mono_ns(&d, counter + 1) < fastix_vdso_mono_ns(&d, counter)) {
                fprintf(stderr, "FAIL seed %llu: time went backwards\n", (unsigned long long)seed);
                return 1;
            }
        }
    }
    printf("test_vdso: %d seeds x %d samples passed\n", SEEDS, SAMPLES);
    return 0;
}
