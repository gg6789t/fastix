/*
 * drivers/rtc_pl031/main.c - isolated ARM PrimeCell PL031 RTC driver.
 * Reads Unix epoch seconds from PL031, updates procd's VDSO realtime offset
 * downward (Rank 20 -> Rank 10), and serves RTC_OP_READ requests.
 */
#include <stdint.h>

#include <fastix/dev_proto.h>
#include <fastix/errno.h>
#include <libdriver/mmio.h>
#include <sel4/sel4.h>

/* PL031 register offsets (ARM DDI 0224). */
#define PL031_DR 0x000U /* 32-bit Unix epoch seconds */
#define PL031_CR 0x00CU /* bit 0 = RTC start */

#define NS_PER_SEC 1000000000ULL

static uint32_t rtc_read_sec(uintptr_t base)
{
    return mmio_read32(base + PL031_DR);
}

static void push_realtime_to_procd(uint64_t epoch_sec)
{
    seL4_SetMR(0, epoch_sec * NS_PER_SEC);
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(FASTIX_SRV_SET_REALTIME_NS, 0, 0, 1);
    (void)seL4_Call(DRV_CAP_PROCD_EP, tag);
}

_Noreturn void drv_main(const struct drv_bootinfo *bi)
{
    uintptr_t base = (uintptr_t)DRV_VADDR_MMIO + bi->mmio_offset;
    mmio_write32(base + PL031_CR, 1U);

    /* Push wall-clock time downward to procd before accepting client calls. */
    push_realtime_to_procd(rtc_read_sec(base));

    seL4_Word badge = 0;
    seL4_MessageInfo_t msg = seL4_Recv(DRV_CAP_SERVICE_EP, &badge, DRV_CAP_REPLY);
    for (;;) {
        seL4_Word op = seL4_MessageInfo_get_label(msg);
        seL4_MessageInfo_t reply;
        if (op == RTC_OP_READ) {
            uint64_t sec = rtc_read_sec(base);
            seL4_SetMR(0, 0);
            seL4_SetMR(1, sec);
            reply = seL4_MessageInfo_new(0, 0, 0, 2);
        } else {
            seL4_SetMR(0, (seL4_Word)(int64_t)-FASTIX_ENOSYS);
            reply = seL4_MessageInfo_new(0, 0, 0, 1);
        }
        msg = seL4_ReplyRecv(DRV_CAP_SERVICE_EP, reply, &badge, DRV_CAP_REPLY);
    }
}
