/* arch/regs.h - AArch64 register context helpers for starting threads. */
#ifndef ARCH_REGS_H
#define ARCH_REGS_H

#include <stdint.h>

#include <sel4/sel4.h>

#define ARCH_REGS_COUNT (sizeof(seL4_UserContext) / sizeof(seL4_Word))

/* Fresh context that starts at `pc` with stack `sp` and first argument `arg0`. */
static inline void arch_regs_init_entry(seL4_UserContext *regs, uint64_t pc, uint64_t sp,
                                        uint64_t arg0)
{
    *regs = (seL4_UserContext){ 0 };
    regs->pc = pc;
    regs->sp = sp;
    regs->x0 = arg0;
}

#endif /* ARCH_REGS_H */
