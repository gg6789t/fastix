/*
 * ticker.h - timer tick helper thread (temporary: replaced by the Phase 4
 * timer driver). It runs in procd's address space on its own small stack,
 * touches no procd data, and only signals procd's timer notification once
 * per MCS scheduling period. procd suspends it whenever no timer is armed.
 */
#ifndef PROCD_TICKER_H
#define PROCD_TICKER_H

#include <sel4/sel4.h>

_Noreturn void ticker_main(seL4_CPtr ntfn);

#endif /* PROCD_TICKER_H */
