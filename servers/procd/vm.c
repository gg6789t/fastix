/* vm.c - frame mapping and per-process object lists (see vm.h). */
#include <arch/vm.h>
#include <fastix/errno.h>
#include <libos/assert.h>
#include <libos/mem.h>

#include "vm.h"

static struct objlist procd_tables; /* page tables of procd's scratch window; kept forever */
static seL4_CPtr scratch_slot;      /* holds a temporary copy of the frame being filled */

int objlist_alloc(struct objlist *list, seL4_Word type, seL4_Word param, seL4_CPtr *cap)
{
    if (list->n == FASTIX_PROCD_MAX_PROC_OBJS) {
        return -FASTIX_ENOMEM;
    }
    int err = kobj_alloc(type, param, &list->obj[list->n]);
    if (err != 0) {
        return err;
    }
    *cap = list->obj[list->n].cap;
    list->n++;
    return 0;
}

void objlist_free_all(struct objlist *list)
{
    while (list->n > 0) {
        kobj_free(&list->obj[--list->n]);
    }
}

int vm_map(struct objlist *owner, seL4_CPtr vspace, seL4_CPtr frame, uint64_t vaddr,
           bool writable, bool exec)
{
    /* Each failed lookup means one missing page-table level; add it and retry. */
    for (int level = 0; level <= ARCH_PT_LEVELS; level++) {
        seL4_Error err = arch_map_page(frame, vspace, vaddr, writable, exec);
        if (err != seL4_FailedLookup) {
            return kobj_sel4_err(err);
        }
        seL4_CPtr pt;
        int e = objlist_alloc(owner, ARCH_PAGE_TABLE_OBJECT, 0, &pt);
        if (e != 0) {
            return e;
        }
        e = kobj_sel4_err(arch_map_page_table(pt, vspace, vaddr));
        if (e != 0) {
            return e;
        }
    }
    return -FASTIX_EINVAL;
}

int vm_init(void)
{
    struct kobj frame;

    int err = kobj_slot_get(&scratch_slot);
    if (err != 0) {
        return err;
    }
    /* Map and unmap one frame so the scratch window's page tables exist from now on. */
    err = kobj_alloc(ARCH_FRAME_OBJECT, 0, &frame);
    if (err != 0) {
        return err;
    }
    err = vm_map(&procd_tables, seL4_CapInitThreadVSpace, frame.cap, FASTIX_PROCD_SCRATCH_VADDR,
                 true, false);
    kobj_free(&frame); /* revoking the frame also removes the mapping */
    return err;
}

int vm_write_frame(seL4_CPtr frame, uint64_t offset, const void *src, size_t len, bool exec)
{
    if (offset > FASTIX_PAGE_SIZE || len > FASTIX_PAGE_SIZE - offset) {
        return -FASTIX_EINVAL;
    }
    /* A frame cap can be mapped only once, so map a temporary copy into procd. */
    int err = kobj_sel4_err(seL4_CNode_Copy(seL4_CapInitThreadCNode, scratch_slot, seL4_WordBits,
                                            seL4_CapInitThreadCNode, frame, seL4_WordBits,
                                            seL4_AllRights));
    if (err != 0) {
        return err;
    }
    err = kobj_sel4_err(arch_map_page(scratch_slot, seL4_CapInitThreadVSpace,
                                      FASTIX_PROCD_SCRATCH_VADDR, true, false));
    if (err == 0) {
        memcpy((void *)(uintptr_t)(FASTIX_PROCD_SCRATCH_VADDR + offset), src, len);
        if (exec) {
            err = kobj_sel4_err(arch_sync_icache(scratch_slot, offset, offset + len));
        }
        if (arch_unmap_page(scratch_slot) != seL4_NoError) {
            libos_assert(0 && "cannot unmap scratch frame");
        }
    }
    if (seL4_CNode_Delete(seL4_CapInitThreadCNode, scratch_slot, seL4_WordBits) != seL4_NoError) {
        libos_assert(0 && "cannot delete scratch copy");
    }
    return err;
}
