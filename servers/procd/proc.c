/* proc.c - user process creation and destruction (see proc.h). */
#include <arch/regs.h>
#include <arch/vm.h>
#include <fastix/config.h>
#include <fastix/cspace.h>
#include <fastix/errno.h>
#include <fastix/vspace.h>
#include <libos/assert.h>
#include <libos/elf.h>

#include "proc.h"

/* Endpoint rights for clients: send, and receive a reply. */
#define CLIENT_EP_RIGHTS seL4_CapRights_new(1, 0, 0, 1) /* grant_reply, grant, read, write */

/* Loads every PT_LOAD segment of `args->elf` into fresh frames. */
static int load_elf(struct proc *p, const struct proc_spawn_args *args, uint64_t *entry)
{
    struct elf_image img;
    bool entry_is_code = false;

    int err = elf_open(&img, args->elf, args->elf_size, ARCH_ELF_MACHINE, FASTIX_EXEC_BASE,
                       FASTIX_EXEC_END);
    if (err != 0) {
        return err;
    }
    for (unsigned i = 0; i < img.phnum; i++) {
        struct elf_segment seg;
        int r = elf_segment(&img, i, &seg);
        if (r <= 0) {
            if (r < 0) {
                return r;
            }
            continue;
        }
        bool exec = (seg.flags & ELF_PF_X) != 0;
        bool writable = (seg.flags & ELF_PF_W) != 0;
        if (exec && img.entry >= seg.vaddr && img.entry - seg.vaddr < seg.memsz) {
            entry_is_code = true;
        }
        uint64_t file_end = seg.vaddr + seg.filesz;
        for (uint64_t page = seg.vaddr & ~(uint64_t)(FASTIX_PAGE_SIZE - 1);
             page < seg.vaddr + seg.memsz; page += FASTIX_PAGE_SIZE) {
            seL4_CPtr frame;
            err = objlist_alloc(&p->objs, ARCH_FRAME_OBJECT, 0, &frame);
            if (err != 0) {
                return err;
            }
            /* File bytes that land in this page; the rest stays zero (fresh frames are zeroed). */
            uint64_t from = page > seg.vaddr ? page : seg.vaddr;
            uint64_t to = page + FASTIX_PAGE_SIZE < file_end ? page + FASTIX_PAGE_SIZE : file_end;
            if (from < to) {
                const uint8_t *src = (const uint8_t *)args->elf + seg.offset + (from - seg.vaddr);
                err = vm_write_frame(frame, from - page, src, (size_t)(to - from), exec);
                if (err != 0) {
                    return err;
                }
            }
            /* Fails if two segments share a page; Phase 1 loader does not merge them. */
            err = vm_map(&p->objs, p->vspace, frame, page, writable, exec);
            if (err != 0) {
                return err;
            }
        }
    }
    *entry = img.entry;
    return entry_is_code ? 0 : -FASTIX_ENOEXEC;
}

/* Allocates a zeroed frame and maps it read-write, no-exec, at `vaddr`. */
static int map_data_page(struct proc *p, uint64_t vaddr, seL4_CPtr *frame)
{
    int err = objlist_alloc(&p->objs, ARCH_FRAME_OBJECT, 0, frame);
    if (err != 0) {
        return err;
    }
    return vm_map(&p->objs, p->vspace, *frame, vaddr, true, false);
}

/* Step 1: address space, CSpace, thread and scheduling context, plus the endpoint cap. */
static int build_objects(struct proc *p, const struct proc_spawn_args *args)
{
    int err;

    if ((err = objlist_alloc(&p->objs, ARCH_VSPACE_OBJECT, 0, &p->vspace)) != 0 ||
        (err = kobj_sel4_err(arch_assign_asid(p->vspace))) != 0 ||
        (err = objlist_alloc(&p->objs, seL4_CapTableObject, FASTIX_ROOT_CNODE_BITS,
                             &p->cnode)) != 0 ||
        (err = objlist_alloc(&p->objs, seL4_TCBObject, 0, &p->tcb)) != 0 ||
        (err = objlist_alloc(&p->objs, seL4_SchedContextObject, seL4_MinSchedContextBits,
                             &p->sc)) != 0) {
        return err;
    }
    /* The only capability the process gets: its badged endpoint to procd. */
    uint64_t badge = fastix_badge_make(FASTIX_BADGE_SYSCALL, p->pid, 0, p->gen);
    return kobj_sel4_err(seL4_CNode_Mint(p->cnode, FASTIX_CAP_PROCD_EP_MAIN, FASTIX_ROOT_CNODE_BITS,
                                         seL4_CapInitThreadCNode, args->endpoint, seL4_WordBits,
                                         CLIENT_EP_RIGHTS, badge));
}

/* Step 2: program, stack and IPC buffer pages. */
static int build_memory(struct proc *p, const struct proc_spawn_args *args, uint64_t *entry,
                        seL4_CPtr *ipcbuf)
{
    seL4_CPtr stack;
    int err;

    if ((err = load_elf(p, args, entry)) != 0 ||
        (err = map_data_page(p, FASTIX_STACK_TOP - FASTIX_PAGE_SIZE, &stack)) != 0) {
        return err;
    }
    return map_data_page(p, FASTIX_IPCBUF_BASE, ipcbuf);
}

/* Step 3: configure the thread, give it CPU time and a fault handler, and start it. */
static int build_thread(struct proc *p, const struct proc_spawn_args *args, uint64_t entry,
                        seL4_CPtr ipcbuf)
{
    /* CSpace guard: a 64-slot root CNode resolves full 64-bit CPtrs. */
    seL4_Word guard = seL4_CNode_CapData_new(0, seL4_WordBits - FASTIX_ROOT_CNODE_BITS).words[0];
    int err = kobj_sel4_err(seL4_TCB_Configure(p->tcb, p->cnode, guard, p->vspace, 0,
                                               FASTIX_IPCBUF_BASE, ipcbuf));
    if (err != 0 ||
        (err = kobj_sel4_err(seL4_SchedControl_ConfigureFlags(
             args->sched_control, p->sc, FASTIX_DEFAULT_TIMESLICE_US,
             FASTIX_DEFAULT_TIMESLICE_US, 0, 0, 0))) != 0) {
        return err;
    }
    /* Faults go to procd with a fault badge. The kernel copies this cap into the TCB. */
    if ((err = kobj_slot_get(&p->fault_ep)) != 0) {
        p->fault_ep = seL4_CapNull;
        return err;
    }
    uint64_t badge = fastix_badge_make(FASTIX_BADGE_FAULT, p->pid, 0, p->gen);
    err = kobj_sel4_err(seL4_CNode_Mint(seL4_CapInitThreadCNode, p->fault_ep, seL4_WordBits,
                                        seL4_CapInitThreadCNode, args->endpoint, seL4_WordBits,
                                        CLIENT_EP_RIGHTS, badge));
    if (err != 0) {
        kobj_slot_put(p->fault_ep);
        p->fault_ep = seL4_CapNull;
        return err;
    }
    err = kobj_sel4_err(seL4_TCB_SetSchedParams(p->tcb, seL4_CapInitThreadTCB, 0,
                                                FASTIX_DEFAULT_PRIO, p->sc, p->fault_ep));
    if (err != 0) {
        return err;
    }
    seL4_UserContext regs;
    arch_regs_init_entry(&regs, entry, FASTIX_STACK_TOP, args->arg0);
    return kobj_sel4_err(seL4_TCB_WriteRegisters(p->tcb, 1, 0, ARCH_REGS_COUNT, &regs));
}

int proc_spawn(struct proc *p, const struct proc_spawn_args *args)
{
    *p = (struct proc){ .pid = args->pid, .gen = args->gen };
    uint64_t entry = 0;
    seL4_CPtr ipcbuf = seL4_CapNull;
    int err = build_objects(p, args);
    if (err == 0) {
        err = build_memory(p, args, &entry, &ipcbuf);
    }
    if (err == 0) {
        err = build_thread(p, args, entry, ipcbuf);
    }
    if (err != 0) {
        proc_destroy(p);
    }
    return err;
}

void proc_destroy(struct proc *p)
{
    if (p->tcb != seL4_CapNull) {
        (void)seL4_TCB_Suspend(p->tcb); /* stop it before its memory goes away */
    }
    if (p->fault_ep != seL4_CapNull) {
        if (seL4_CNode_Delete(seL4_CapInitThreadCNode, p->fault_ep, seL4_WordBits) !=
            seL4_NoError) {
            libos_assert(0 && "cannot delete fault endpoint copy");
        }
        kobj_slot_put(p->fault_ep);
        p->fault_ep = seL4_CapNull;
    }
    /* Revoking each object's block removes every cap derived from it, everywhere. */
    objlist_free_all(&p->objs);
}
