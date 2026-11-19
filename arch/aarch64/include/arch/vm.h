/*
 * arch/vm.h - AArch64 wrappers for seL4 page mapping.
 * Callers handle seL4_FailedLookup by mapping a page table and retrying.
 */
#ifndef ARCH_VM_H
#define ARCH_VM_H

#include <stdbool.h>

#include <sel4/sel4.h>

#define ARCH_ELF_MACHINE          183  /* EM_AARCH64 */
#define ARCH_PT_LEVELS            3    /* page-table levels below the VSpace root */
#define ARCH_FRAME_OBJECT         seL4_ARM_SmallPageObject
#define ARCH_PAGE_TABLE_OBJECT    seL4_ARM_PageTableObject
#define ARCH_VSPACE_OBJECT        seL4_ARM_VSpaceObject

static inline seL4_Error arch_map_page(seL4_CPtr frame, seL4_CPtr vspace, seL4_Word vaddr,
                                       bool writable, bool exec)
{
    seL4_ARM_VMAttributes attr = seL4_ARM_Default_VMAttributes;

    if (!exec) {
        attr = (seL4_ARM_VMAttributes)(attr | seL4_ARM_ExecuteNever);
    }
    return seL4_ARM_Page_Map(frame, vspace, vaddr, writable ? seL4_ReadWrite : seL4_CanRead,
                             attr);
}

static inline seL4_Error arch_unmap_page(seL4_CPtr frame)
{
    return seL4_ARM_Page_Unmap(frame);
}

static inline seL4_Error arch_map_page_table(seL4_CPtr pt, seL4_CPtr vspace, seL4_Word vaddr)
{
    return seL4_ARM_PageTable_Map(pt, vspace, vaddr, seL4_ARM_Default_VMAttributes);
}

static inline seL4_Error arch_assign_asid(seL4_CPtr vspace)
{
    return seL4_ARM_ASIDPool_Assign(seL4_CapInitThreadASIDPool, vspace);
}

/* Make freshly written code in a mapped frame visible to instruction fetch. */
static inline seL4_Error arch_sync_icache(seL4_CPtr mapped_frame, seL4_Word start, seL4_Word end)
{
    return seL4_ARM_Page_Unify_Instruction(mapped_frame, start, end);
}

#endif /* ARCH_VM_H */
