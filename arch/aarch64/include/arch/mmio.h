/*
 * arch/mmio.h - AArch64 memory barriers and volatile MMIO accessors.
 */
#ifndef ARCH_MMIO_H
#define ARCH_MMIO_H

#include <stdint.h>

static inline void arch_mb(void)
{
    __asm__ volatile("dsb sy" : : : "memory");
}

static inline void arch_rmb(void)
{
    __asm__ volatile("dsb ld" : : : "memory");
}

static inline void arch_wmb(void)
{
    __asm__ volatile("dsb st" : : : "memory");
}

static inline uint32_t mmio_read32(uintptr_t addr)
{
    uint32_t v = *(const volatile uint32_t *)addr;
    arch_rmb();
    return v;
}

static inline void mmio_write32(uintptr_t addr, uint32_t v)
{
    arch_wmb();
    *(volatile uint32_t *)addr = v;
}

#endif /* ARCH_MMIO_H */
