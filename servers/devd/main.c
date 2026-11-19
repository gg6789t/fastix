/*
 * servers/devd/main.c - device manager and driver supervisor (Rank 20).
 * Parses the DTB, probes VirtIO-MMIO slots, launches isolated drivers with
 * least-privilege caps, registers their endpoints downward with procd, and
 * supervises driver faults in a single-threaded event loop.
 */
#include <stdbool.h>
#include <stdint.h>

#include <fastix/config.h>
#include <fastix/dev_proto.h>
#include <fastix/errno.h>
#include <libdriver/dtb.h>
#include <libdriver/mmio.h>
#include <libdriver/virtio.h>
#include <libos/kprint.h>
#include <libos/mem.h>
#include <sel4/sel4.h>

#include "devd_internal.h"

static struct drv_inst drivers[FASTIX_DRV_COUNT];
static struct dkobj server_reply;
static uint32_t dtb_total_size;
static uint32_t active_drv_count;

void devd_entry(const struct devd_bootinfo *bi)
{
    seL4_SetIPCBuffer((seL4_IPCBuffer *)(uintptr_t)DEVD_VADDR_IPCBUF);
    devd_main(bi);
}

static void fill_simple_spec(struct drv_spec *sp, const struct dtb_view *v,
                             const char *compat, uint32_t kind, bool bind_irq, bool rtc_procd)
{
    struct dtb_node n;
    uint64_t base = 0, size = 0;
    struct dtb_irq irq;

    if (dtb_find_compat(v, compat, 0, &n) != 0 ||
        dtb_node_reg(v, &n, 0, &base, &size) != 0) {
        return;
    }
    sp->kind = kind;
    sp->present = true;
    sp->has_mmio = true;
    sp->mmio_paddr = base;
    sp->mmio_offset = (uint32_t)(base & (FASTIX_PAGE_SIZE - 1U));
    sp->mmio_size = (uint32_t)size;
    sp->bind_irq_ntfn = bind_irq;
    sp->grant_rtc_procd = rtc_procd;
    if (dtb_node_irq(v, &n, 0, &irq) == 0) {
        sp->has_irq = true;
        sp->irq_num = irq.irq;
        sp->irq_edge = irq.edge;
    }
}

/* Walks all virtio,mmio DTB nodes, probes Magic/Version/DeviceID, and records
 * the first VirtIO-RNG (id 4) and VirtIO-BLK (id 2) devices found. */
static void probe_virtio(const struct dtb_view *v)
{
    struct dtb_node n;
    size_t cursor = 0;

    while (dtb_find_compat(v, "virtio,mmio", cursor, &n) == 0) {
        cursor = n.next_off;
        uint64_t base = 0, size = 0;
        struct dtb_irq irq;
        if (dtb_node_reg(v, &n, 0, &base, &size) != 0 ||
            dtb_node_irq(v, &n, 0, &irq) != 0) {
            continue;
        }
        uint64_t page_pa = base & ~(uint64_t)(FASTIX_PAGE_SIZE - 1U);
        uint32_t off = (uint32_t)(base & (FASTIX_PAGE_SIZE - 1U));
        seL4_CPtr frame = 0;
        if (devd_mmio_frame(page_pa, &frame) != 0 ||
            devd_map_scratch(frame, true) != 0) {
            continue;
        }
        uintptr_t va = (uintptr_t)DEVD_VADDR_SCRATCH + off;
        uint32_t magic = mmio_read32(va + VIRTIO_MMIO_MAGIC);
        uint32_t ver = mmio_read32(va + VIRTIO_MMIO_VERSION);
        uint32_t dev_id = mmio_read32(va + VIRTIO_MMIO_DEVICE_ID);
        devd_unmap_scratch(frame);

        if (magic != VIRTIO_MAGIC_VALUE ||
            (ver != VIRTIO_VERSION_LEGACY && ver != VIRTIO_VERSION_MODERN)) {
            continue;
        }
        uint32_t kind = FASTIX_DRV_COUNT;
        uint32_t dma_cnt = 0;
        if (dev_id == VIRTIO_DEV_RNG && !drivers[FASTIX_DRV_RNG].spec.present) {
            kind = FASTIX_DRV_RNG;
            dma_cnt = 1;
        } else if (dev_id == VIRTIO_DEV_BLK && !drivers[FASTIX_DRV_BLK].spec.present) {
            kind = FASTIX_DRV_BLK;
            dma_cnt = 2;
        }
        if (kind < FASTIX_DRV_COUNT) {
            drivers[kind].spec = (struct drv_spec){
                .kind = kind,
                .present = true,
                .has_mmio = true,
                .has_irq = true,
                .irq_edge = irq.edge,
                .mmio_paddr = base,
                .mmio_offset = off,
                .mmio_size = (uint32_t)size,
                .irq_num = irq.irq,
                .dma_count = dma_cnt,
            };
        }
    }
}

/* Registers a driver's service_ep downward with procd (Rank 20 -> Rank 10). */
static void register_drv_with_procd(uint32_t kind, seL4_CPtr ep)
{
    seL4_SetCap(0, ep);
    seL4_SetMR(0, kind);
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(FASTIX_SRV_REGISTER_DRV, 0, 1, 1);
    (void)seL4_Call(DEVD_CAP_PROCD_EP, tag);
}

static seL4_MessageInfo_t handle_client(seL4_MessageInfo_t msg)
{
    seL4_Word op = seL4_MessageInfo_get_label(msg);
    seL4_Word len = seL4_MessageInfo_get_length(msg);

    if (op == DEVD_OP_INFO) {
        uint64_t free_bytes = 0, slots_used = 0;
        devd_kobj_stats(&free_bytes, &slots_used);
        seL4_SetMR(0, 0);
        seL4_SetMR(1, active_drv_count);
        seL4_SetMR(2, free_bytes);
        seL4_SetMR(3, slots_used);
        seL4_SetMR(4, dtb_total_size);
        return seL4_MessageInfo_new(0, 0, 0, 5);
    }

    if (op == DEVD_OP_DRV_STATUS) {
        if (len < 1U) {
            seL4_SetMR(0, (seL4_Word)(int64_t)-FASTIX_EINVAL);
            return seL4_MessageInfo_new(0, 0, 0, 1);
        }
        uint64_t kind = seL4_GetMR(0);
        if (kind >= FASTIX_DRV_COUNT) {
            seL4_SetMR(0, (seL4_Word)(int64_t)-FASTIX_EINVAL);
            return seL4_MessageInfo_new(0, 0, 0, 1);
        }
        const struct drv_inst *d = &drivers[kind];
        seL4_SetMR(0, 0);
        seL4_SetMR(1, d->running ? 1U : 0U);
        seL4_SetMR(2, d->restarts);
        seL4_SetMR(3, d->spec.mmio_paddr);
        seL4_SetMR(4, d->spec.has_irq ? d->spec.irq_num : 0U);
        return seL4_MessageInfo_new(0, 0, 0, 5);
    }

    seL4_SetMR(0, (seL4_Word)(int64_t)-FASTIX_ENOSYS);
    return seL4_MessageInfo_new(0, 0, 0, 1);
}

static void handle_driver_fault(uint32_t kind)
{
    if (kind >= FASTIX_DRV_COUNT) {
        return;
    }
    struct drv_inst *d = &drivers[kind];
    const struct drv_image *img = drv_image_get(kind);
    kprint_str("devd: driver faulted (");
    kprint_str(img != NULL ? img->name : "?");
    kprint_str("), restarting...\n");

    int err = drv_restart(d);
    if (err != 0) {
        kprint_str("devd: restart failed, err=-");
        kprint_u64((uint64_t)-err);
        kprint_str("\n");
    } else {
        kprint_str("devd: restarted ");
        kprint_str(img != NULL ? img->name : "?");
        kprint_str(" (gen=");
        kprint_u64(d->restarts);
        kprint_str(")\n");
    }
}

_Noreturn void devd_main(const struct devd_bootinfo *bi)
{
    if (devd_kobj_init(bi) != 0 || dkobj_alloc(seL4_ReplyObject, 0, &server_reply) != 0) {
        kprint_str("devd: fatal init failure\n");
        for (;;) {
        }
    }

    struct dtb_view dtb;
    if (dtb_validate((const void *)(uintptr_t)bi->dtb_vaddr, bi->dtb_size, &dtb) != 0) {
        kprint_str("devd: invalid DTB\n");
        for (;;) {
        }
    }
    dtb_total_size = dtb.totalsize;

    fill_simple_spec(&drivers[FASTIX_DRV_UART].spec, &dtb, "arm,pl011",
                     FASTIX_DRV_UART, true, false);
    fill_simple_spec(&drivers[FASTIX_DRV_RTC].spec, &dtb, "arm,pl031",
                     FASTIX_DRV_RTC, false, true);
    probe_virtio(&dtb);

    /* Always start the isolated fault_test driver (no MMIO/IRQ/DMA caps). */
    drivers[FASTIX_DRV_FAULT].spec = (struct drv_spec){
        .kind = FASTIX_DRV_FAULT,
        .present = true,
    };

    for (uint32_t k = 0; k < FASTIX_DRV_COUNT; k++) {
        if (!drivers[k].spec.present) {
            continue;
        }
        int err = drv_start_first(&drivers[k]);
        if (err == 0) {
            active_drv_count++;
            register_drv_with_procd(k, drivers[k].service_ep.cap);
        } else {
            const struct drv_image *img = drv_image_get(k);
            kprint_str("devd: failed to start ");
            kprint_str(img != NULL ? img->name : "?");
            kprint_str("\n");
        }
    }

    kprint_str("devd: started ");
    kprint_u64(active_drv_count);
    kprint_str(" isolated drivers\n");

    seL4_Word badge = 0;
    seL4_MessageInfo_t msg = seL4_Recv(DEVD_CAP_SERVICE_EP, &badge, server_reply.cap);
    for (;;) {
        uint64_t btype = badge >> DEVD_BADGE_TYPE_SHIFT;
        if (btype == DEVD_BADGE_FAULT) {
            handle_driver_fault((uint32_t)(badge & DEVD_BADGE_ID_MASK));
            /* Do not reply to the dead faulted TCB; wait for the next message. */
            msg = seL4_Recv(DEVD_CAP_SERVICE_EP, &badge, server_reply.cap);
            continue;
        }
        seL4_MessageInfo_t reply = handle_client(msg);
        msg = seL4_ReplyRecv(DEVD_CAP_SERVICE_EP, reply, &badge, server_reply.cap);
    }
}
