/*
 * arch/qemu_exit.h - stop QEMU with an exit code (test builds only).
 * Uses Arm semihosting, which QEMU honors from user mode only when started with
 * -semihosting-config enable=on,userspace=on. Elsewhere it faults instead.
 */
#ifndef ARCH_QEMU_EXIT_H
#define ARCH_QEMU_EXIT_H

#include <stdint.h>

#define SEMIHOST_SYS_EXIT          0x18
#define SEMIHOST_ADP_APP_EXIT      0x20026  /* ADP_Stopped_ApplicationExit */

static inline void arch_qemu_exit(int code)
{
    uint64_t block[2] = { SEMIHOST_ADP_APP_EXIT, (uint64_t)(int64_t)code };
    register uint64_t op __asm__("x0") = SEMIHOST_SYS_EXIT;
    register uint64_t arg __asm__("x1") = (uint64_t)(uintptr_t)block;

    __asm__ volatile("hlt #0xf000" : "+r"(op) : "r"(arg) : "memory");
}

#endif /* ARCH_QEMU_EXIT_H */
