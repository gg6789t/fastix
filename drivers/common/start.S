/* drivers/common/start.S - entry point for isolated Fastix drivers. */
    .section .text.start, "ax"
    .global _start
_start:
    bl drv_entry
1:  wfe
    b 1b
