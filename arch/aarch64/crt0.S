/*
 * crt0.S - first instructions of the Fastix root task (procd) on AArch64.
 * seL4 starts the root task with x0 = pointer to seL4_BootInfo and no stack.
 */
#include <fastix/config.h>

    .section .text._sel4_start, "ax"
    .global _sel4_start
    .type _sel4_start, %function
_sel4_start:
    adrp    x1, root_stack_top          /* switch to our static stack */
    add     x1, x1, :lo12:root_stack_top
    mov     sp, x1
    bl      procd_main                  /* x0 still holds the BootInfo pointer */
1:  wfi                                 /* procd_main never returns */
    b       1b
    .size _sel4_start, . - _sel4_start

    .section .bss.root_stack, "aw", %nobits
    .balign 16
root_stack:
    .space FASTIX_SERVER_STACK_SIZE
root_stack_top:
