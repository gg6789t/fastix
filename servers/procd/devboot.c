/*
 * devboot.c - spawns isolated devd from procd and manages registered driver EPs.
 */
#include <stdbool.h>
#include <stdint.h>

#include <arch/regs.h>
#include <arch/vm.h>
#include <fastix/config.h>
#include <fastix/cspace.h>
#include <fastix/dev_proto.h>
#include <fastix/errno.h>
#include <fastix/vspace.h>
#include <libos/elf.h>
#include <libos/mem.h>
#include <sel4/sel4.h>

#include "cspace.h"
#include "devboot.h"
#include "frame.h"
#include "kobj.h"
#include "proc.h"
#include "server.h"
#include "vdso.h"

extern const uint8_t fastix_image_devd_start[], fastix_image_devd_end[];

#define DEVD_STACK_PAGES (FASTIX_DEVD_STACK_SIZE / FASTIX_PAGE_SIZE)
#define DEVD_MAX_PTS     16

static struct kobj devd_vspace;
static struct kobj devd_cnode;
static struct kobj devd_tcb;
static struct kobj devd_sc;
static struct kobj devd_service_ep;
static struct kobj devd_ram_ut;
static struct kobj devd_pts[DEVD_MAX_PTS];
static size_t devd_n_pts;

static seL4_CPtr srv_recv_slot;
static seL4_CPtr drv_ep_slots[FASTIX_DRV_COUNT];

static int map_into_devd(seL4_CPtr frame, uint64_t vaddr, bool writable, bool exec)
{
    for (int lvl = 0; lvl <= ARCH_PT_LEVELS; lvl++) {
        seL4_Error err = arch_map_page(frame, devd_vspace.cap, vaddr, writable, exec);
        if (err == seL4_NoError) {
            return 0;
        }
        if (err != seL4_FailedLookup || lvl == ARCH_PT_LEVELS || devd_n_pts >= DEVD_MAX_PTS) {
            return kobj_sel4_err(err);
        }
        int aerr = kobj_alloc(ARCH_PAGE_TABLE_OBJECT, 0, &devd_pts[devd_n_pts]);
        if (aerr != 0) {
            return aerr;
        }
        err = arch_map_page_table(devd_pts[devd_n_pts].cap, devd_vspace.cap, vaddr);
        if (err != seL4_NoError) {
            kobj_free(&devd_pts[devd_n_pts]);
            return kobj_sel4_err(err);
        }
        devd_n_pts++;
    }
    return -FASTIX_EINVAL;
}

static int copy_to_devd(seL4_CPtr dst_slot, seL4_CPtr src_cap)
{
    seL4_Error err = seL4_CNode_Copy(devd_cnode.cap, dst_slot, FASTIX_DEVD_CSPACE_BITS,
                                     cspace_root(), src_cap, seL4_WordBits, seL4_AllRights);
    return kobj_sel4_err(err);
}

static int move_to_devd(seL4_CPtr dst_slot, seL4_CPtr src_cap)
{
    seL4_Error err = seL4_CNode_Move(devd_cnode.cap, dst_slot, FASTIX_DEVD_CSPACE_BITS,
                                     cspace_root(), src_cap, seL4_WordBits);
    return kobj_sel4_err(err);
}

static int mint_procd_ep_to_devd(seL4_CPtr dst_slot, uint32_t srvid)
{
    uint64_t badge = fastix_badge_make(FASTIX_BADGE_SERVER, srvid, 0, 0);
    seL4_Error err = seL4_CNode_Mint(devd_cnode.cap, dst_slot, FASTIX_DEVD_CSPACE_BITS,
                                     cspace_root(), server_endpoint(), seL4_WordBits,
                                     seL4_AllRights, badge);
    return kobj_sel4_err(err);
}

/* Locates SEL4_BOOTINFO_HEADER_FDT in seL4's extra bootinfo region. */
static int find_bootinfo_dtb(const seL4_BootInfo *bi, const uint8_t **dtb_out, uint32_t *size_out)
{
    if (bi->extraLen < sizeof(seL4_BootInfoHeader)) {
        return -FASTIX_ENOENT;
    }
    const uint8_t *base = (const uint8_t *)bi + seL4_BootInfoFrameSize;
    size_t off = 0;

    while (off + sizeof(seL4_BootInfoHeader) <= bi->extraLen) {
        const seL4_BootInfoHeader *hdr = (const seL4_BootInfoHeader *)(const void *)(base + off);
        if (hdr->len < sizeof(seL4_BootInfoHeader) || hdr->len > bi->extraLen - off) {
            return -FASTIX_EINVAL;
        }
        if (hdr->id == SEL4_BOOTINFO_HEADER_FDT) {
            size_t payload = hdr->len - sizeof(seL4_BootInfoHeader);
            if (payload == 0 || payload > DEVD_VADDR_DTB_MAX_SIZE) {
                return -FASTIX_EINVAL;
            }
            *dtb_out = base + off + sizeof(seL4_BootInfoHeader);
            *size_out = (uint32_t)payload;
            return 0;
        }
        off += hdr->len;
    }
    return -FASTIX_ENOENT;
}

static int load_devd_elf(uint64_t *entry_out)
{
    size_t elf_size = (size_t)(fastix_image_devd_end - fastix_image_devd_start);
    struct elf_image eimg;

    int err = elf_open(&eimg, fastix_image_devd_start, elf_size, ARCH_ELF_MACHINE);
    if (err != 0) {
        return err;
    }
    err = elf_set_load(&eimg, 0, FASTIX_EXEC_BASE, DEVD_VADDR_IPCBUF);
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
            struct frame *f = NULL;
            err = frame_new(&f);
            if (err != 0) {
                return err;
            }
            uint64_t file_end = seg.vaddr + seg.filesz;
            uint64_t lo = va > seg.vaddr ? va : seg.vaddr;
            uint64_t hi = (va + FASTIX_PAGE_SIZE) < file_end ? (va + FASTIX_PAGE_SIZE) : file_end;
            if (hi > lo) {
                size_t dst_off = (size_t)(lo - va);
                size_t src_off = (size_t)(seg.offset + (lo - seg.vaddr));
                err = frame_write(f, dst_off, fastix_image_devd_start + src_off,
                                  (size_t)(hi - lo), exec);
                if (err != 0) {
                    return err;
                }
            }
            err = map_into_devd(f->cap, va, writable, exec);
            if (err != 0) {
                return err;
            }
        }
    }
    *entry_out = eimg.entry;
    return 0;
}

int devboot_start(const seL4_BootInfo *bi)
{
    const uint8_t *dtb_ptr = NULL;
    uint32_t dtb_size = 0;
    int err = find_bootinfo_dtb(bi, &dtb_ptr, &dtb_size);
    if (err != 0) {
        return err;
    }

    if ((err = kobj_slot_get(&srv_recv_slot)) != 0 ||
        (err = kobj_alloc(ARCH_VSPACE_OBJECT, 0, &devd_vspace)) != 0 ||
        (err = kobj_alloc(seL4_CapTableObject, FASTIX_DEVD_CSPACE_BITS, &devd_cnode)) != 0 ||
        (err = kobj_alloc(seL4_TCBObject, 0, &devd_tcb)) != 0 ||
        (err = kobj_alloc(seL4_SchedContextObject, seL4_MinSchedContextBits, &devd_sc)) != 0 ||
        (err = kobj_alloc(seL4_EndpointObject, 0, &devd_service_ep)) != 0 ||
        (err = kobj_alloc(seL4_UntypedObject, FASTIX_DEVD_RAM_UT_BITS, &devd_ram_ut)) != 0) {
        return err;
    }

    /* Set procd's IPC cap receive slot so devd can transfer driver service_ep caps. */
    seL4_SetCapReceivePath(cspace_root(), srv_recv_slot, seL4_WordBits);

    if ((err = kobj_sel4_err(arch_assign_asid(devd_vspace.cap))) != 0) {
        return err;
    }

    uint64_t entry = 0;
    struct frame *ipcbuf = NULL;
    struct frame *bi_frame = NULL;
    if ((err = load_devd_elf(&entry)) != 0 ||
        (err = frame_new(&ipcbuf)) != 0 ||
        (err = map_into_devd(ipcbuf->cap, DEVD_VADDR_IPCBUF, true, false)) != 0 ||
        (err = frame_new(&bi_frame)) != 0) {
        return err;
    }

    /* Copy runtime DTB pages into devd read-only at DEVD_VADDR_DTB. */
    for (uint32_t off = 0; off < dtb_size; off += FASTIX_PAGE_SIZE) {
        struct frame *df = NULL;
        uint32_t chunk = dtb_size - off;
        if (chunk > FASTIX_PAGE_SIZE) {
            chunk = FASTIX_PAGE_SIZE;
        }
        if ((err = frame_new(&df)) != 0 ||
            (err = frame_write(df, 0, dtb_ptr + off, chunk, false)) != 0 ||
            (err = map_into_devd(df->cap, DEVD_VADDR_DTB + off, false, false)) != 0) {
            return err;
        }
    }

    /* Map devd stack. */
    for (size_t i = 0; i < DEVD_STACK_PAGES; i++) {
        struct frame *sf = NULL;
        uint64_t va = DEVD_VADDR_STACK_TOP - (i + 1U) * FASTIX_PAGE_SIZE;
        if ((err = frame_new(&sf)) != 0 ||
            (err = map_into_devd(sf->cap, va, true, false)) != 0) {
            return err;
        }
    }

    /* Populate devd's fixed CSpace slots. Mint DEVD_CAP_SELF_CNODE with the
     * 52-bit zero guard so lookups with depth seL4_WordBits resolve directly. */
    seL4_Word guard = seL4_CNode_CapData_new(0, seL4_WordBits - FASTIX_DEVD_CSPACE_BITS).words[0];
    if ((err = kobj_sel4_err(seL4_CNode_Mint(devd_cnode.cap, DEVD_CAP_SELF_CNODE,
                                             FASTIX_DEVD_CSPACE_BITS, cspace_root(),
                                             devd_cnode.cap, seL4_WordBits,
                                             seL4_AllRights, guard))) != 0 ||
        (err = copy_to_devd(DEVD_CAP_SELF_VSPACE, devd_vspace.cap)) != 0 ||
        (err = copy_to_devd(DEVD_CAP_SELF_TCB, devd_tcb.cap)) != 0 ||
        (err = copy_to_devd(DEVD_CAP_SERVICE_EP, devd_service_ep.cap)) != 0 ||
        (err = mint_procd_ep_to_devd(DEVD_CAP_PROCD_EP, FASTIX_SRVID_DEVD)) != 0 ||
        (err = mint_procd_ep_to_devd(DEVD_CAP_RTC_PROCD_EP, FASTIX_SRVID_RTC)) != 0 ||
        (err = move_to_devd(DEVD_CAP_IRQ_CONTROL, seL4_CapIRQControl)) != 0 ||
        (err = copy_to_devd(DEVD_CAP_SCHED_CONTROL, bi->schedcontrol.start)) != 0 ||
        (err = copy_to_devd(DEVD_CAP_ASID_POOL, seL4_CapInitThreadASIDPool)) != 0 ||
        (err = copy_to_devd(DEVD_CAP_RAM_UT, devd_ram_ut.cap)) != 0) {
        return err;
    }

    /* Move all Device Untypeds from procd's boot CNode into devd. */
    struct devd_bootinfo dbi;
    memset(&dbi, 0, sizeof(dbi));
    dbi.ram_ut_paddr = devd_ram_ut.block->paddr;
    dbi.ram_ut_bits = FASTIX_DEVD_RAM_UT_BITS;
    dbi.dtb_vaddr = DEVD_VADDR_DTB;
    dbi.dtb_size = dtb_size;

    seL4_Word ut_count = bi->untyped.end - bi->untyped.start;
    for (seL4_Word i = 0; i < ut_count; i++) {
        const seL4_UntypedDesc *ut = &bi->untypedList[i];
        if (!ut->isDevice || dbi.dev_ut_count >= FASTIX_DEVD_MAX_DEV_UTS) {
            continue;
        }
        uint32_t slot = DEVD_CAP_DEV_UT_FIRST + dbi.dev_ut_count;
        err = move_to_devd(slot, bi->untyped.start + i);
        if (err != 0) {
            return err;
        }
        dbi.dev_uts[dbi.dev_ut_count] = (struct devd_ut_desc){
            .paddr = ut->paddr,
            .size_bits = ut->sizeBits,
            .slot = slot,
        };
        dbi.dev_ut_count++;
    }

    if ((err = frame_write(bi_frame, 0, &dbi, sizeof(dbi), false)) != 0 ||
        (err = map_into_devd(bi_frame->cap, DEVD_VADDR_BOOTINFO, false, false)) != 0) {
        return err;
    }

    /* Configure and start devd (mcp == prio so devd can set driver priorities). */
    if ((err = kobj_sel4_err(seL4_TCB_Configure(devd_tcb.cap, devd_cnode.cap, guard,
                                                devd_vspace.cap, 0, DEVD_VADDR_IPCBUF,
                                                ipcbuf->cap))) != 0 ||
        (err = kobj_sel4_err(seL4_SchedControl_ConfigureFlags(
             bi->schedcontrol.start, devd_sc.cap,
             FASTIX_DEFAULT_TIMESLICE_US, FASTIX_DEFAULT_TIMESLICE_US, 0, 0, 0))) != 0 ||
        (err = kobj_sel4_err(seL4_TCB_SetSchedParams(devd_tcb.cap, seL4_CapInitThreadTCB,
                                                     FASTIX_DEVD_PRIO, FASTIX_DEVD_PRIO,
                                                     devd_sc.cap, seL4_CapNull))) != 0) {
        return err;
    }

    seL4_UserContext regs;
    arch_regs_init_entry(&regs, entry, DEVD_VADDR_STACK_TOP, DEVD_VADDR_BOOTINFO);
    return kobj_sel4_err(seL4_TCB_WriteRegisters(devd_tcb.cap, true, 0, ARCH_REGS_COUNT, &regs));
}

int devboot_install_devd_ep(struct proc *p)
{
    if (devd_service_ep.cap == seL4_CapNull) {
        return -FASTIX_ENOENT;
    }
    seL4_Error err = seL4_CNode_Copy(p->cnode.cap, FASTIX_CAP_DEVD_EP, FASTIX_ROOT_CNODE_BITS,
                                     cspace_root(), devd_service_ep.cap, seL4_WordBits,
                                     seL4_AllRights);
    return kobj_sel4_err(err);
}

int devboot_install_drv_ep(struct proc *p, uint32_t kind)
{
    if (kind >= FASTIX_DRV_COUNT || drv_ep_slots[kind] == seL4_CapNull) {
        return -FASTIX_ENOENT;
    }
    seL4_CPtr dst = FASTIX_CAP_DRV_FIRST + kind;
    (void)seL4_CNode_Delete(p->cnode.cap, dst, FASTIX_ROOT_CNODE_BITS);
    seL4_Error err = seL4_CNode_Copy(p->cnode.cap, dst, FASTIX_ROOT_CNODE_BITS,
                                     cspace_root(), drv_ep_slots[kind], seL4_WordBits,
                                     seL4_AllRights);
    return kobj_sel4_err(err);
}

void devboot_handle_server_msg(seL4_Word badge, seL4_MessageInfo_t info, unsigned *nret)
{
    uint32_t srvid = fastix_badge_pid(badge);
    seL4_Word op = seL4_MessageInfo_get_label(info);
    seL4_Word len = seL4_MessageInfo_get_length(info);
    int64_t rc = -FASTIX_ENOSYS;

    if (op == FASTIX_SRV_SET_REALTIME_NS) {
        if ((srvid != FASTIX_SRVID_RTC && srvid != FASTIX_SRVID_DEVD) || len < 1U) {
            rc = -FASTIX_EPERM;
        } else {
            vdso_set_realtime_ns(seL4_GetMR(0));
            rc = 0;
        }
    } else if (op == FASTIX_SRV_REGISTER_DRV) {
        if (srvid != FASTIX_SRVID_DEVD) {
            rc = -FASTIX_EPERM;
        } else if (len < 1U || seL4_MessageInfo_get_extraCaps(info) != 1U) {
            rc = -FASTIX_EINVAL;
        } else {
            uint64_t kind = seL4_GetMR(0);
            if (kind >= FASTIX_DRV_COUNT) {
                rc = -FASTIX_EINVAL;
            } else {
                seL4_CPtr dst = drv_ep_slots[kind];
                if (dst == seL4_CapNull) {
                    int err = kobj_slot_get(&dst);
                    if (err != 0) {
                        rc = err;
                        goto out_cleanup;
                    }
                    drv_ep_slots[kind] = dst;
                } else {
                    (void)seL4_CNode_Delete(cspace_root(), dst, seL4_WordBits);
                }
                seL4_Error se = seL4_CNode_Move(cspace_root(), dst, seL4_WordBits,
                                                cspace_root(), srv_recv_slot, seL4_WordBits);
                rc = kobj_sel4_err(se);
            }
        }
    out_cleanup:
        /* Ensure receive slot is empty for the next capability transfer. */
        if (rc != 0 && seL4_MessageInfo_get_extraCaps(info) > 0) {
            (void)seL4_CNode_Delete(cspace_root(), srv_recv_slot, seL4_WordBits);
        }
    }

    seL4_SetMR(0, (seL4_Word)rc);
    *nret = 1;
}
