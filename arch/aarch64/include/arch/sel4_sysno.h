/*
 * arch/sel4_sysno.h - seL4 system call numbers needed by assembly code.
 * Assembly cannot use libsel4's enum, so the value is repeated here once and
 * checked against libsel4 at compile time in servers/procd/vdso.c.
 */
#ifndef ARCH_SEL4_SYSNO_H
#define ARCH_SEL4_SYSNO_H

#define ARCH_SEL4_SYS_CALL (-1) /* seL4_SysCall */

#endif /* ARCH_SEL4_SYSNO_H */
