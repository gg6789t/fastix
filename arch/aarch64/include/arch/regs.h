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

/*
 * Turns the saved context of a thread blocked in seL4_Call into one that has
 * just returned from that call with a 1-word reply `mr0` (used to start a fork
 * child). On AArch64 the saved pc is the svc instruction itself (seL4 stores
 * ELR - 4 on syscall entry), and libsel4 reads the badge from x0, the message
 * info from x1, and MR0 from x2.
 */
static inline void arch_regs_return_from_call(seL4_UserContext *regs, uint64_t mr0)
{
    regs->pc += 4;
    regs->x0 = 0;
    regs->x1 = seL4_MessageInfo_new(0, 0, 0, 1).words[0];
    regs->x2 = mr0;
}

#endif /* ARCH_REGS_H */
