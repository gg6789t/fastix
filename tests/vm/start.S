/*
 * start.S - entry point of the Phase 2 test programs. The loader leaves sp
 * pointing at argc (System V layout); pass it to prog_main.
 */
    .section .text._start, "ax"
    .global _start
    .type _start, %function
_start:
    mov     x0, sp
    mov     x29, xzr
    mov     x30, xzr
    bl      prog_main
1:  b       1b
    .size _start, . - _start
