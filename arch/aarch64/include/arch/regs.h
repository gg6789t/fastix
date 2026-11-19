/* arch/regs.h - AArch64 register context helpers for starting and resuming threads. */
#ifndef ARCH_REGS_H
#define ARCH_REGS_H

#include <stdint.h>

#include <fastix/signal_abi.h>
#include <sel4/sel4.h>

#define ARCH_REGS_COUNT (sizeof(seL4_UserContext) / sizeof(seL4_Word))

/* Read-only thread slot register set by procd (0 for main thread, 1..1023 for workers). */
static inline uint64_t arch_read_tpidrro(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v));
    return v;
}

/* Writable POSIX TLS pointer (tpidr_el0). */
static inline uint64_t arch_read_tpidr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

/* Fresh context that starts at `pc` with stack `sp`, first argument `arg0`, and `tid_slot`. */
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
 * just returned from that call with 1..4 reply words (used by fork, vfork,
 * and signal delivery on syscall return). On AArch64 the saved pc is the svc
 * instruction itself (seL4 stores ELR - 4 on syscall entry), and libsel4 reads
 * badge from x0, message info from x1, and MR0..MR3 from x2..x5.
 */
static inline void arch_regs_return_from_call_n(seL4_UserContext *regs, unsigned n,
                                                const uint64_t *mr)
{
    regs->pc += 4;
    regs->x0 = 0;
    regs->x1 = seL4_MessageInfo_new(0, 0, 0, n).words[0];
    regs->x2 = n > 0 ? mr[0] : 0;
    regs->x3 = n > 1 ? mr[1] : 0;
    regs->x4 = n > 2 ? mr[2] : 0;
    regs->x5 = n > 3 ? mr[3] : 0;
}

static inline void arch_regs_return_from_call(seL4_UserContext *regs, uint64_t mr0)
{
    arch_regs_return_from_call_n(regs, 1, &mr0);
}

static inline void arch_regs_to_sigctx(const seL4_UserContext *r, struct fastix_sigcontext *sc)
{
    sc->x[0] = r->x0;   sc->x[1] = r->x1;   sc->x[2] = r->x2;   sc->x[3] = r->x3;
    sc->x[4] = r->x4;   sc->x[5] = r->x5;   sc->x[6] = r->x6;   sc->x[7] = r->x7;
    sc->x[8] = r->x8;   sc->x[9] = r->x9;   sc->x[10] = r->x10; sc->x[11] = r->x11;
    sc->x[12] = r->x12; sc->x[13] = r->x13; sc->x[14] = r->x14; sc->x[15] = r->x15;
    sc->x[16] = r->x16; sc->x[17] = r->x17; sc->x[18] = r->x18; sc->x[19] = r->x19;
    sc->x[20] = r->x20; sc->x[21] = r->x21; sc->x[22] = r->x22; sc->x[23] = r->x23;
    sc->x[24] = r->x24; sc->x[25] = r->x25; sc->x[26] = r->x26; sc->x[27] = r->x27;
    sc->x[28] = r->x28; sc->x[29] = r->x29; sc->x[30] = r->x30;
    sc->sp = r->sp;
    sc->pc = r->pc;
    sc->pstate = r->spsr;
    sc->tpidr_el0 = r->tpidr_el0;
}

static inline void arch_regs_from_sigctx(seL4_UserContext *r, const struct fastix_sigcontext *sc)
{
    r->x0 = sc->x[0];   r->x1 = sc->x[1];   r->x2 = sc->x[2];   r->x3 = sc->x[3];
    r->x4 = sc->x[4];   r->x5 = sc->x[5];   r->x6 = sc->x[6];   r->x7 = sc->x[7];
    r->x8 = sc->x[8];   r->x9 = sc->x[9];   r->x10 = sc->x[10]; r->x11 = sc->x[11];
    r->x12 = sc->x[12]; r->x13 = sc->x[13]; r->x14 = sc->x[14]; r->x15 = sc->x[15];
    r->x16 = sc->x[16]; r->x17 = sc->x[17]; r->x18 = sc->x[18]; r->x19 = sc->x[19];
    r->x20 = sc->x[20]; r->x21 = sc->x[21]; r->x22 = sc->x[22]; r->x23 = sc->x[23];
    r->x24 = sc->x[24]; r->x25 = sc->x[25]; r->x26 = sc->x[26]; r->x27 = sc->x[27];
    r->x28 = sc->x[28]; r->x29 = sc->x[29]; r->x30 = sc->x[30];
    r->sp = sc->sp;
    r->pc = sc->pc;
    r->spsr = sc->pstate;
    r->tpidr_el0 = sc->tpidr_el0;
}

#endif /* ARCH_REGS_H */
