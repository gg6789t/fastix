/*
 * servers/devd/spawn.c - builds, tears down, and restarts isolated driver PDs.
 * Every driver gets its own VSpace, CSpace, TCB, SchedContext, IPC buffer,
 * stack, and ONLY its own MMIO/DMA/IRQ capabilities.
 */
#include <arch/regs.h>
#include <arch/vm.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <fastix/vspace.h>
#include <libos/elf.h>
#include <libos/kprint.h>
#include <libos/mem.h>

#include "devd_internal.h"

#define DRV_STACK_PAGES (FASTIX_DRV_STACK_SIZE / FASTIX_PAGE_SIZE)

static int map_into_drv(struct drv_inst *d, seL4_CPtr frame, uint64_t vaddr,
                        bool writable, bool exec, bool is_mmio)
{
    for (int lvl = 0; lvl <= ARCH_PT_LEVELS; lvl++) {
        seL4_Error err = is_mmio
            ? arch_map_mmio_page(frame, d->vspace.cap, vaddr)
            : arch_map_page(frame, d->vspace.cap, vaddr, writable, exec);
        if (err == seL4_NoError) {
            return 0;
        }
        if (err != seL4_FailedLookup || lvl == ARCH_PT_LEVELS || d->n_pts >= DEVD_MAX_DRV_PTS) {
            return -FASTIX_ENOMEM;
        }
        int aerr = dkobj_alloc(ARCH_PAGE_TABLE_OBJECT, 0, &d->pts[d->n_pts]);
        if (aerr != 0) {
            return aerr;
        }
        err = arch_map_page_table(d->pts[d->n_pts].cap, d->vspace.cap, vaddr);
        if (err != seL4_NoError) {
            dkobj_free(&d->pts[d->n_pts]);
            return -FASTIX_EINVAL;
        }
        d->n_pts++;
    }
    return -FASTIX_EINVAL;
}

static int copy_cap_to_drv(struct drv_inst *d, seL4_CPtr dst_slot, seL4_CPtr src_cap,
                           seL4_CapRights_t rights)
{
    seL4_Error err = seL4_CNode_Copy(d->cnode.cap, dst_slot, FASTIX_DRV_CNODE_BITS,
                                     DEVD_CAP_SELF_CNODE, src_cap, seL4_WordBits, rights);
    return err == seL4_NoError ? 0 : -FASTIX_EINVAL;
}

static int mint_cap_to_drv(struct drv_inst *d, seL4_CPtr dst_slot, seL4_CPtr src_cap,
                           seL4_CapRights_t rights, seL4_Word badge)
{
    seL4_Error err = seL4_CNode_Mint(d->cnode.cap, dst_slot, FASTIX_DRV_CNODE_BITS,
                                     DEVD_CAP_SELF_CNODE, src_cap, seL4_WordBits,
                                     rights, badge);
    return err == seL4_NoError ? 0 : -FASTIX_EINVAL;
}

/* Copies bytes into a freshly allocated driver frame via devd's scratch page. */
static int write_frame(seL4_CPtr frame, size_t off, const void *src, size_t len, bool sync_icache)
{
    int err = devd_map_scratch(frame, false);
    if (err != 0) {
        return err;
    }
    uint8_t *dst = (uint8_t *)(uintptr_t)DEVD_VADDR_SCRATCH;
    if (src != NULL && len > 0) {
        memcpy(dst + off, src, len);
    }
    if (sync_icache) {
        (void)arch_sync_icache(frame, 0, FASTIX_PAGE_SIZE);
    }
    devd_unmap_scratch(frame);
    return 0;
}

/* Loads all PT_LOAD segments of a static driver ELF into d->vspace. */
static int load_drv_elf(struct drv_inst *d, const struct drv_image *img, uint64_t *entry_out)
{
    size_t elf_size = (size_t)(img->end - img->start);
    struct elf_image eimg;

    int err = elf_open(&eimg, img->start, elf_size, ARCH_ELF_MACHINE);
    if (err != 0) {
        return err;
    }
    err = elf_set_load(&eimg, 0, FASTIX_EXEC_BASE, DRV_VADDR_IPCBUF);
    if (err != 0) {
        return err;
    }

    for (unsigned s = 0; s < eimg.phnum; s++) {
        struct elf_segment seg;
        int r = elf_segment(&eimg, s, &seg);
        if (r < 0) {
            return r;
        }
        if (r == 0) {
            continue;
        }
        uint64_t start_pg = seg.vaddr & ~(uint64_t)(FASTIX_PAGE_SIZE - 1U);
        uint64_t end_pg = (seg.vaddr + seg.memsz + FASTIX_PAGE_SIZE - 1U) &
                          ~(uint64_t)(FASTIX_PAGE_SIZE - 1U);
        bool writable = (seg.flags & ELF_PF_W) != 0;
        bool exec = (seg.flags & ELF_PF_X) != 0;

        for (uint64_t va = start_pg; va < end_pg; va += FASTIX_PAGE_SIZE) {
            if (d->n_frames >= DEVD_MAX_DRV_FRAMES) {
                return -FASTIX_ENOMEM;
            }
            struct dkobj *fr = &d->frames[d->n_frames];
            err = dkobj_alloc(ARCH_FRAME_OBJECT, 0, fr);
            if (err != 0) {
                return err;
            }
            d->n_frames++;

            uint64_t file_end = seg.vaddr + seg.filesz;
            uint64_t copy_lo = va > seg.vaddr ? va : seg.vaddr;
            uint64_t copy_hi = (va + FASTIX_PAGE_SIZE) < file_end
                                   ? (va + FASTIX_PAGE_SIZE)
                                   : file_end;
            if (copy_hi > copy_lo) {
                size_t dst_off = (size_t)(copy_lo - va);
                size_t src_off = (size_t)(seg.offset + (copy_lo - seg.vaddr));
                size_t clen = (size_t)(copy_hi - copy_lo);
                err = write_frame(fr->cap, dst_off, img->start + src_off, clen, exec);
                if (err != 0) {
                    return err;
                }
            }
            err = map_into_drv(d, fr->cap, va, writable, exec, false);
            if (err != 0) {
                return err;
            }
        }
    }

    *entry_out = eimg.entry;
    return 0;
}

static void drv_teardown_gen(struct drv_inst *d)
{
    if (d->tcb.cap != seL4_CapNull) {
        (void)seL4_TCB_Suspend(d->tcb.cap);
    }
    if (d->spec.has_irq && d->irq_master_cap != seL4_CapNull) {
        (void)seL4_IRQHandler_Clear(d->irq_master_cap);
    }
    (void)dslot_delete_and_put(d->fault_ep_badged);
    d->fault_ep_badged = seL4_CapNull;

    dkobj_free(&d->tcb);
    dkobj_free(&d->sc);
    dkobj_free(&d->reply);
    dkobj_free(&d->irq_ntfn);
    dkobj_free(&d->cnode);
    dkobj_free(&d->bootinfo);
    dkobj_free(&d->ipcbuf);
    for (uint32_t i = 0; i < 2; i++) {
        dkobj_free(&d->dma[i]);
    }
    while (d->n_frames > 0) {
        dkobj_free(&d->frames[--d->n_frames]);
    }
    while (d->n_pts > 0) {
        dkobj_free(&d->pts[--d->n_pts]);
    }
    dkobj_free(&d->vspace);
    d->running = false;
}

static int drv_build_and_run(struct drv_inst *d)
{
    const struct drv_image *img = drv_image_get(d->spec.kind);
    if (img == NULL) {
        return -FASTIX_EINVAL;
    }

    int err = 0;
    uint64_t entry = 0;

    if ((err = dkobj_alloc(ARCH_VSPACE_OBJECT, 0, &d->vspace)) != 0 ||
        (err = dkobj_alloc(seL4_CapTableObject, FASTIX_DRV_CNODE_BITS, &d->cnode)) != 0 ||
        (err = dkobj_alloc(seL4_TCBObject, 0, &d->tcb)) != 0 ||
        (err = dkobj_alloc(seL4_SchedContextObject, seL4_MinSchedContextBits, &d->sc)) != 0 ||
        (err = dkobj_alloc(seL4_ReplyObject, 0, &d->reply)) != 0 ||
        (err = dkobj_alloc(ARCH_FRAME_OBJECT, 0, &d->ipcbuf)) != 0 ||
        (err = dkobj_alloc(ARCH_FRAME_OBJECT, 0, &d->bootinfo)) != 0) {
        goto fail;
    }

    if (seL4_ARM_ASIDPool_Assign(DEVD_CAP_ASID_POOL, d->vspace.cap) != seL4_NoError) {
        err = -FASTIX_EINVAL;
        goto fail;
    }

    /* Load ELF, map IPC buffer, read-only bootinfo, and stack. */
    if ((err = load_drv_elf(d, img, &entry)) != 0 ||
        (err = map_into_drv(d, d->ipcbuf.cap, DRV_VADDR_IPCBUF, true, false, false)) != 0) {
        goto fail;
    }

    struct drv_bootinfo dbi = {
        .kind = d->spec.kind,
        .restart_gen = d->restarts,
        .mmio_paddr = d->spec.mmio_paddr,
        .mmio_offset = d->spec.mmio_offset,
        .mmio_size = d->spec.mmio_size,
        .irq = d->spec.has_irq ? d->spec.irq_num : 0,
        .dma_count = d->spec.dma_count,
    };
    if ((err = write_frame(d->bootinfo.cap, 0, &dbi, sizeof(dbi), false)) != 0 ||
        (err = map_into_drv(d, d->bootinfo.cap, DRV_VADDR_BOOTINFO, false, false, false)) != 0) {
        goto fail;
    }

    for (size_t i = 0; i < DRV_STACK_PAGES; i++) {
        if (d->n_frames >= DEVD_MAX_DRV_FRAMES) {
            err = -FASTIX_ENOMEM;
            goto fail;
        }
        struct dkobj *spg = &d->frames[d->n_frames];
        if ((err = dkobj_alloc(ARCH_FRAME_OBJECT, 0, spg)) != 0) {
            goto fail;
        }
        d->n_frames++;
        uint64_t va = DRV_VADDR_STACK_TOP - (i + 1U) * FASTIX_PAGE_SIZE;
        if ((err = map_into_drv(d, spg->cap, va, true, false, false)) != 0) {
            goto fail;
        }
    }

    /* Map device MMIO frame via a temporary copy, then move it into the driver's CSpace
     * so tearing down d->cnode unmaps DRV_VADDR_MMIO while keeping d->mmio_master_cap intact. */
    if (d->spec.has_mmio) {
        seL4_CPtr tmp_mmio = 0;
        if ((err = dslot_get(&tmp_mmio)) != 0) {
            goto fail;
        }
        if (seL4_CNode_Copy(DEVD_CAP_SELF_CNODE, tmp_mmio, seL4_WordBits,
                            DEVD_CAP_SELF_CNODE, d->mmio_master_cap, seL4_WordBits,
                            seL4_AllRights) != seL4_NoError) {
            dslot_put(tmp_mmio);
            err = -FASTIX_EINVAL;
            goto fail;
        }
        err = map_into_drv(d, tmp_mmio, DRV_VADDR_MMIO, true, false, true);
        if (err != 0) {
            (void)dslot_delete_and_put(tmp_mmio);
            goto fail;
        }
        if (seL4_CNode_Move(d->cnode.cap, DRV_CAP_MMIO_FRAME, FASTIX_DRV_CNODE_BITS,
                            DEVD_CAP_SELF_CNODE, tmp_mmio, seL4_WordBits) != seL4_NoError) {
            (void)dslot_delete_and_put(tmp_mmio);
            err = -FASTIX_EINVAL;
            goto fail;
        }
        dslot_put(tmp_mmio);
    }

    /* Allocate & map DMA frames: map a copy and Move it into the driver's CSpace so
     * DRV_CAP_DMA_FRAME_i holds the mapped cap (required by seL4_ARM_Page_Clean/Invalidate). */
    for (uint32_t i = 0; i < d->spec.dma_count && i < 2; i++) {
        uint64_t va = (i == 0) ? DRV_VADDR_DMA_0 : DRV_VADDR_DMA_1;
        seL4_CPtr cslot = (i == 0) ? DRV_CAP_DMA_FRAME_0 : DRV_CAP_DMA_FRAME_1;
        if ((err = dkobj_alloc(ARCH_FRAME_OBJECT, 0, &d->dma[i])) != 0) {
            goto fail;
        }
        seL4_CPtr tmp_dma = 0;
        if ((err = dslot_get(&tmp_dma)) != 0) {
            goto fail;
        }
        if (seL4_CNode_Copy(DEVD_CAP_SELF_CNODE, tmp_dma, seL4_WordBits,
                            DEVD_CAP_SELF_CNODE, d->dma[i].cap, seL4_WordBits,
                            seL4_AllRights) != seL4_NoError) {
            dslot_put(tmp_dma);
            err = -FASTIX_EINVAL;
            goto fail;
        }
        err = map_into_drv(d, tmp_dma, va, true, false, false);
        if (err != 0) {
            (void)dslot_delete_and_put(tmp_dma);
            goto fail;
        }
        if (seL4_CNode_Move(d->cnode.cap, cslot, FASTIX_DRV_CNODE_BITS,
                            DEVD_CAP_SELF_CNODE, tmp_dma, seL4_WordBits) != seL4_NoError) {
            (void)dslot_delete_and_put(tmp_dma);
            err = -FASTIX_EINVAL;
            goto fail;
        }
        dslot_put(tmp_dma);
    }

    /* Populate driver's minimal CSpace. */
    if ((err = copy_cap_to_drv(d, DRV_CAP_SERVICE_EP, d->service_ep.cap, seL4_AllRights)) != 0 ||
        (err = copy_cap_to_drv(d, DRV_CAP_REPLY, d->reply.cap, seL4_AllRights)) != 0) {
        goto fail;
    }
    if (d->spec.has_irq) {
        if ((err = dkobj_alloc(seL4_NotificationObject, 0, &d->irq_ntfn)) != 0 ||
            (err = mint_cap_to_drv(d, DRV_CAP_IRQ_NTFN, d->irq_ntfn.cap, seL4_AllRights, 1U)) != 0 ||
            (err = copy_cap_to_drv(d, DRV_CAP_IRQ_HANDLER, d->irq_master_cap, seL4_AllRights)) != 0) {
            goto fail;
        }
    }
    if (d->spec.grant_rtc_procd) {
        if ((err = copy_cap_to_drv(d, DRV_CAP_PROCD_EP, DEVD_CAP_RTC_PROCD_EP, seL4_AllRights)) != 0) {
            goto fail;
        }
    }

    /* Mint badged fault endpoint so seL4 reports faults for this driver to devd. */
    if ((err = dslot_get(&d->fault_ep_badged)) != 0) {
        goto fail;
    }
    uint64_t fbadge = (DEVD_BADGE_FAULT << DEVD_BADGE_TYPE_SHIFT) | (uint64_t)d->spec.kind;
    if (seL4_CNode_Mint(DEVD_CAP_SELF_CNODE, d->fault_ep_badged, seL4_WordBits,
                        DEVD_CAP_SELF_CNODE, DEVD_CAP_SERVICE_EP, seL4_WordBits,
                        seL4_AllRights, fbadge) != seL4_NoError) {
        dslot_put(d->fault_ep_badged);
        d->fault_ep_badged = seL4_CapNull;
        err = -FASTIX_EINVAL;
        goto fail;
    }

    /* Configure TCB, SchedContext, optional bound IRQ notification, and registers. */
    seL4_Word guard = seL4_CNode_CapData_new(0, seL4_WordBits - FASTIX_DRV_CNODE_BITS).words[0];
    if (seL4_TCB_Configure(d->tcb.cap, d->cnode.cap, guard, d->vspace.cap, 0,
                           DRV_VADDR_IPCBUF, d->ipcbuf.cap) != seL4_NoError ||
        seL4_SchedControl_ConfigureFlags(DEVD_CAP_SCHED_CONTROL, d->sc.cap,
                                         FASTIX_DEFAULT_TIMESLICE_US, FASTIX_DEFAULT_TIMESLICE_US,
                                         0, 0, 0) != seL4_NoError ||
        seL4_TCB_SetSchedParams(d->tcb.cap, DEVD_CAP_SELF_TCB, 0,
                                FASTIX_DRV_PRIO, d->sc.cap, d->fault_ep_badged) != seL4_NoError) {
        err = -FASTIX_EINVAL;
        goto fail;
    }
    if (d->spec.has_irq && d->spec.bind_irq_ntfn) {
        if (seL4_TCB_BindNotification(d->tcb.cap, d->irq_ntfn.cap) != seL4_NoError) {
            err = -FASTIX_EINVAL;
            goto fail;
        }
    }

    seL4_UserContext regs;
    arch_regs_init_entry(&regs, entry, DRV_VADDR_STACK_TOP, DRV_VADDR_BOOTINFO);
    if (seL4_TCB_WriteRegisters(d->tcb.cap, true, 0, ARCH_REGS_COUNT, &regs) != seL4_NoError) {
        err = -FASTIX_EINVAL;
        goto fail;
    }

    d->running = true;
    return 0;

fail:
    drv_teardown_gen(d);
    return err;
}

int drv_start_first(struct drv_inst *d)
{
    int err = dkobj_alloc(seL4_EndpointObject, 0, &d->service_ep);
    if (err != 0) {
        return err;
    }
    if (d->spec.has_mmio) {
        uint64_t page_pa = d->spec.mmio_paddr & ~(uint64_t)(FASTIX_PAGE_SIZE - 1U);
        err = devd_mmio_frame(page_pa, &d->mmio_master_cap);
        if (err != 0) {
            return err;
        }
    }
    if (d->spec.has_irq) {
        err = dslot_get(&d->irq_master_cap);
        if (err != 0) {
            return err;
        }
        seL4_Error se = seL4_IRQControl_GetTrigger(DEVD_CAP_IRQ_CONTROL, d->spec.irq_num,
                                                   d->spec.irq_edge ? 1U : 0U,
                                                   DEVD_CAP_SELF_CNODE, d->irq_master_cap,
                                                   seL4_WordBits);
        if (se != seL4_NoError) {
            dslot_put(d->irq_master_cap);
            d->irq_master_cap = seL4_CapNull;
            return -FASTIX_EINVAL;
        }
    }
    return drv_build_and_run(d);
}

void drv_unblock_caller_eio(struct drv_inst *d)
{
    if (d->reply.cap == seL4_CapNull) {
        return;
    }
    seL4_SetMR(0, (seL4_Word)(int64_t)-FASTIX_EIO);
    seL4_NBSend(d->reply.cap, seL4_MessageInfo_new(0, 0, 0, 1));
}

int drv_restart(struct drv_inst *d)
{
    drv_unblock_caller_eio(d);
    drv_teardown_gen(d);
    if (d->restarts >= FASTIX_DRV_MAX_RESTARTS) {
        return -FASTIX_EIO;
    }
    d->restarts++;
    return drv_build_and_run(d);
}
