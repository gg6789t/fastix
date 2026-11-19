/*
 * fastix/vdso.h - read-only pages every process can read without IPC
 * (AGENTS.md 5.7): the global clock data page at FASTIX_VDSO_ADDR and the
 * per-process info page at FASTIX_PROCINFO_ADDR.
 *
 * Seqlock protocol: the writer makes `seq` odd, updates the fields, then
 * makes `seq` even. A reader retries while `seq` is odd or changed.
 */
#ifndef FASTIX_VDSO_H
#define FASTIX_VDSO_H

#include <stdatomic.h>
#include <stdint.h>

struct fastix_vdso_data {
    _Atomic uint32_t seq;
    uint32_t shift;              /* ns = mono_base_ns + ((counter - counter_base) * mult >> shift) */
    uint64_t mult;
    uint64_t counter_base;
    uint64_t mono_base_ns;
    uint64_t realtime_offset_ns; /* CLOCK_REALTIME = CLOCK_MONOTONIC + this */
    uint64_t boottime_offset_ns; /* CLOCK_BOOTTIME = CLOCK_MONOTONIC + this */
    int32_t tz_offset_sec;
    uint32_t reserved;
};

struct fastix_procinfo {
    _Atomic uint32_t seq;
    uint32_t pid;
    uint32_t ppid;
    uint32_t pgid;
    uint32_t sid;
    uint32_t uid;
    uint32_t euid;
    uint32_t gid;
    uint32_t egid;
    uint32_t umask;
};

#define FASTIX_VDSO_SHIFT 32

/* mult for a counter running at `freq_hz` (1 Hz .. 4.29 GHz): 1e9 * 2^32 fits in 64 bits. */
static inline uint64_t fastix_vdso_mult(uint64_t freq_hz)
{
    return ((1000000000ULL << FASTIX_VDSO_SHIFT) + freq_hz / 2) / freq_hz;
}

/* Converts a counter value to CLOCK_MONOTONIC nanoseconds (64x64->128-bit multiply, no division). */
static inline uint64_t fastix_vdso_mono_ns(const struct fastix_vdso_data *d, uint64_t counter)
{
    __extension__ typedef unsigned __int128 u128;
    u128 delta = (u128)(counter - d->counter_base) * d->mult;

    return d->mono_base_ns + (uint64_t)(delta >> d->shift);
}

#endif /* FASTIX_VDSO_H */
