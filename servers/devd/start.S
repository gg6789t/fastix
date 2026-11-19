/* servers/devd/start.S - entry point for devd. */
    .section .text.start, "ax"
    .global _start
_start:
    bl devd_entry
1:  wfe
    b 1b
