/*
 * vdso_tramp.S - AArch64 VDSO code page (mapped R-X at FASTIX_VDSO_CODE_ADDR).
 *
 * Layout:
 *   0x000: minimal ELF64 shared-object header (AT_SYSINFO_EHDR)
 *   0x100: __fastix_sigenter  (saves q0..q31, fpsr, fpcr; jumps to handler)
 *   0x200: __fastix_sigreturn (restores q0..q31, fpsr, fpcr; calls rt_sigreturn)
 */
#include <fastix/signal_abi.h>

    .section .rodata.vdso_code, "a"
    .balign 4096
    .global arch_vdso_code_start
arch_vdso_code_start:
    /* Minimal ELF64 ET_DYN header for EM_AARCH64 (64 bytes). */
    .byte   0x7f, 'E', 'L', 'F' /* e_ident: magic */
    .byte   2, 1, 1, 0          /* ELFCLASS64, ELFDATA2LSB, EV_CURRENT, ELFOSABI_NONE */
    .zero   8                   /* padding */
    .hword  3                   /* e_type = ET_DYN */
    .hword  183                 /* e_machine = EM_AARCH64 */
    .word   1                   /* e_version = EV_CURRENT */
    .quad   0                   /* e_entry */
    .quad   0                   /* e_phoff */
    .quad   0                   /* e_shoff */
    .word   0                   /* e_flags */
    .hword  64                  /* e_ehsize */
    .hword  56                  /* e_phentsize */
    .hword  0                   /* e_phnum */
    .hword  64                  /* e_shentsize */
    .hword  0                   /* e_shnum */
    .hword  0                   /* e_shstrndx */

    .org    FASTIX_VDSO_SIGENTER_OFF
    /*
     * Entry from procd signal delivery:
     *   sp  = struct fastix_sigframe *
     *   x0  = signo, x1 = &frame->info, x2 = &frame->uc, x3 = handler
     *   x30 = FASTIX_VDSO_CODE_ADDR + FASTIX_VDSO_SIGRETURN_OFF
     */
    add     x9, sp, #FASTIX_SIGFRAME_FPU_OFF
    stp     q0,  q1,  [x9, #0]
    stp     q2,  q3,  [x9, #32]
    stp     q4,  q5,  [x9, #64]
    stp     q6,  q7,  [x9, #96]
    stp     q8,  q9,  [x9, #128]
    stp     q10, q11, [x9, #160]
    stp     q12, q13, [x9, #192]
    stp     q14, q15, [x9, #224]
    stp     q16, q17, [x9, #256]
    stp     q18, q19, [x9, #288]
    stp     q20, q21, [x9, #320]
    stp     q22, q23, [x9, #352]
    stp     q24, q25, [x9, #384]
    stp     q26, q27, [x9, #416]
    stp     q28, q29, [x9, #448]
    stp     q30, q31, [x9, #480]
    mrs     x10, fpsr
    mrs     x11, fpcr
    str     w10, [x9, #512]
    str     w11, [x9, #516]
    br      x3

    .org    FASTIX_VDSO_SIGRETURN_OFF
    /*
     * Return from user signal handler (x30 landed here with sp == frame):
     * restores all FPU/SIMD registers, then invokes FASTIX_SYS_rt_sigreturn.
     */
    add     x9, sp, #FASTIX_SIGFRAME_FPU_OFF
    ldp     q0,  q1,  [x9, #0]
    ldp     q2,  q3,  [x9, #32]
    ldp     q4,  q5,  [x9, #64]
    ldp     q6,  q7,  [x9, #96]
    ldp     q8,  q9,  [x9, #128]
    ldp     q10, q11, [x9, #160]
    ldp     q12, q13, [x9, #192]
    ldp     q14, q15, [x9, #224]
    ldp     q16, q17, [x9, #256]
    ldp     q18, q19, [x9, #288]
    ldp     q20, q21, [x9, #320]
    ldp     q22, q23, [x9, #352]
    ldp     q24, q25, [x9, #384]
    ldp     q26, q27, [x9, #416]
    ldp     q28, q29, [x9, #448]
    ldp     q30, q31, [x9, #480]
    ldr     w10, [x9, #512]
    ldr     w11, [x9, #516]
    msr     fpsr, x10
    msr     fpcr, x11
    ldr     x0, [sp, #FASTIX_SIGFRAME_EP_OFF]
    movz    x1, #(FASTIX_SIGRETURN_MSGINFO & 0xFFFF)
    movk    x1, #((FASTIX_SIGRETURN_MSGINFO >> 16) & 0xFFFF), lsl #16
    mov     x2, sp
    mov     x7, #-1             /* seL4_SysCall */
    svc     #0
1:  b       1b

    .global arch_vdso_code_end
arch_vdso_code_end:
