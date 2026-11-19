/*
 * start.S - vfsd entry. procd starts vfsd like a program (System V stack at sp);
 * vfsd ignores argc/argv and enters vfsd_main.
 */
    .section .text._start, "ax"
    .global _start
    .type _start, %function
_start:
    mov     x29, xzr
    mov     x30, xzr
    bl      vfsd_main
1:  b       1b
    .size _start, . - _start
