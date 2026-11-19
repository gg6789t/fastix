/*
 * child.c - the Phase 1 test child: an unprivileged process whose only
 * capability is its badged endpoint to procd. It sends `pings` PING calls,
 * reports the result, then faults on purpose so procd can destroy it.
 */
#include <stdint.h>

#include <arch/counter.h>
#include <fastix/cspace.h>
#include <fastix/vspace.h>
#include <sel4/sel4.h>

#include "ipc_test_proto.h"

_Noreturn void child_start(uint64_t pings);

_Noreturn void child_start(uint64_t pings)
{
    seL4_SetIPCBuffer((seL4_IPCBuffer *)FASTIX_IPCBUF_BASE);

    uint64_t wrong = 0;
    uint64_t start = arch_read_counter();
    for (uint64_t i = 0; i < pings; i++) {
        seL4_SetMR(0, i);
        seL4_MessageInfo_t reply =
            seL4_Call(FASTIX_CAP_PROCD_EP_MAIN, seL4_MessageInfo_new(TEST_OP_PING, 0, 0, 1));
        if (seL4_MessageInfo_get_length(reply) != 1 || seL4_GetMR(0) != i + 1) {
            wrong++;
        }
    }
    uint64_t ticks = arch_read_counter() - start;

    seL4_SetMR(0, wrong);
    seL4_SetMR(1, ticks);
    (void)seL4_Call(FASTIX_CAP_PROCD_EP_MAIN, seL4_MessageInfo_new(TEST_OP_DONE, 0, 0, 2));

    /* Controlled fault: procd receives it on our fault endpoint and destroys us. */
    (void)*(volatile const uint64_t *)TEST_FAULT_ADDR;
    for (;;) {
    }
}
