/*
 * drivers/fault_test/main.c - isolated test driver that deliberately faults on
 * FAULT_OP_CRASH so tests can prove devd fault isolation and automatic restart.
 */
#include <stdint.h>

#include <fastix/dev_proto.h>
#include <fastix/errno.h>
#include <sel4/sel4.h>

_Noreturn void drv_main(const struct drv_bootinfo *bi)
{
    seL4_Word badge = 0;
    seL4_MessageInfo_t msg = seL4_Recv(DRV_CAP_SERVICE_EP, &badge, DRV_CAP_REPLY);
    for (;;) {
        seL4_Word op = seL4_MessageInfo_get_label(msg);
        seL4_MessageInfo_t reply;

        if (op == FAULT_OP_PING) {
            uint64_t v = seL4_MessageInfo_get_length(msg) > 0 ? seL4_GetMR(0) : 0;
            seL4_SetMR(0, 0);
            seL4_SetMR(1, v + 1U);
            seL4_SetMR(2, bi->restart_gen);
            reply = seL4_MessageInfo_new(0, 0, 0, 3);
        } else if (op == FAULT_OP_CRASH) {
            /* Deliberate unmapped-memory write mid-call: seL4 delivers a fault
             * to devd, which must unblock the caller with -EIO and restart us. */
            *(volatile uint32_t *)(uintptr_t)0x10 = 0xDEADBEEFU;
            seL4_SetMR(0, 0);
            reply = seL4_MessageInfo_new(0, 0, 0, 1);
        } else {
            seL4_SetMR(0, (seL4_Word)(int64_t)-FASTIX_ENOSYS);
            reply = seL4_MessageInfo_new(0, 0, 0, 1);
        }
        msg = seL4_ReplyRecv(DRV_CAP_SERVICE_EP, reply, &badge, DRV_CAP_REPLY);
    }
}
