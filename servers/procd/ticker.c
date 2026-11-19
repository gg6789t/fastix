/* ticker.c - timer tick helper thread (see ticker.h). */
#include "ticker.h"

_Noreturn void ticker_main(seL4_CPtr ntfn)
{
    for (;;) {
        seL4_Signal(ntfn); /* register-only syscalls: no IPC buffer needed */
        seL4_Yield();      /* MCS: give up the rest of this period's budget until the next refill */
    }
}
