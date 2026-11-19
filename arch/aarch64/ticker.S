/*
 * ticker.S - minimal MCS periodic timer-tick helper (touches zero memory/stack).
 * Started with x0 = badged Notification CPtr bound to procd's main thread.
 * Signals the notification once per MCS scheduling period, then yields its
 * remaining budget so seL4 puts it on the release queue until the next tick.
 */
    .section .text.arch_ticker_entry, "ax"
    .global arch_ticker_entry
    .type arch_ticker_entry, %function
arch_ticker_entry:
    mov     x19, x0
1:
    mov     x0, x19
    mov     x1, #0
    mov     x7, #-5     /* seL4_SysSend -> seL4_Signal on a Notification */
    svc     #0
    mov     x7, #-11    /* seL4_SysYield -> sleep until next MCS refill */
    svc     #0
    b       1b
    .size arch_ticker_entry, . - arch_ticker_entry
