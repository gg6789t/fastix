/*
 * arch/counter.h - read the Arm generic timer's virtual counter from user mode.
 * Needs KernelArmExportVCNTUser=ON (config/aarch64-qemu-virt.cmake).
 */
#ifndef ARCH_COUNTER_H
#define ARCH_COUNTER_H

#include <stdint.h>

/* Current counter value. The isb keeps the read from moving earlier. */
static inline uint64_t arch_read_counter(void)
{
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
}

/* Counter ticks per second. */
static inline uint64_t arch_counter_freq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

#endif /* ARCH_COUNTER_H */
