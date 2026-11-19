/* kobj.c - procd kernel object allocator (see kobj.h). */
#include <arch/vm.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <libos/assert.h>
#include <libos/slot_alloc.h>

#include "kobj.h"

static uint64_t slot_bits[SLOT_ALLOC_WORDS(FASTIX_PROCD_MAX_SLOTS)];
static struct slot_alloc slots;
static struct ut_block ut_nodes[FASTIX_PROCD_UT_NODES];
static struct ut_buddy buddy;

int kobj_sel4_err(seL4_Error err)
{
    switch (err) {
    case seL4_NoError:
        return 0;
    case seL4_NotEnoughMemory:
        return -FASTIX_ENOMEM;
    default:
        return -FASTIX_EINVAL;
    }
}

static int backend_split(void *ctx, uint64_t parent, uint64_t slot, unsigned bits)
{
    (void)ctx;
    return kobj_sel4_err(seL4_Untyped_Retype(parent, seL4_UntypedObject, bits,
                                             seL4_CapInitThreadCNode, 0, 0, slot, 1));
}

static int backend_revoke(void *ctx, uint64_t parent)
{
    (void)ctx;
    return kobj_sel4_err(seL4_CNode_Revoke(seL4_CapInitThreadCNode, parent, seL4_WordBits));
}

int kobj_init(const seL4_BootInfo *bi)
{
    seL4_Word empty = bi->empty.end - bi->empty.start;

    if (empty > FASTIX_PROCD_MAX_SLOTS) {
        empty = FASTIX_PROCD_MAX_SLOTS; /* manage what our bitmap can hold */
    }
    slot_alloc_init(&slots, bi->empty.start, empty, slot_bits);
    ut_buddy_init(&buddy, ut_nodes, FASTIX_PROCD_UT_NODES, &slots,
                  (struct ut_backend){ backend_split, backend_revoke, NULL });

    for (seL4_Word i = 0; i < bi->untyped.end - bi->untyped.start; i++) {
        const seL4_UntypedDesc *ut = &bi->untypedList[i];
        if (ut->isDevice) {
            continue; /* device memory is for devd (Phase 4) */
        }
        int err = ut_buddy_add(&buddy, bi->untyped.start + i, ut->paddr, ut->sizeBits);
        if (err != 0) {
            return err;
        }
    }
    return 0;
}

/* Size in bits of an object of `type`, or 0 for an unsupported type. */
static unsigned object_bits(seL4_Word type, seL4_Word param)
{
    switch (type) {
    case seL4_TCBObject:
        return seL4_TCBBits;
    case seL4_EndpointObject:
        return seL4_EndpointBits;
    case seL4_NotificationObject:
        return seL4_NotificationBits;
    case seL4_ReplyObject:
        return seL4_ReplyBits;
    case seL4_SchedContextObject:
    case seL4_UntypedObject:
        return (unsigned)param;
    case seL4_CapTableObject:
        return (unsigned)param + seL4_SlotBits;
    case ARCH_FRAME_OBJECT:
        return seL4_PageBits;
    case ARCH_PAGE_TABLE_OBJECT:
        return seL4_PageTableBits;
    case ARCH_VSPACE_OBJECT:
        return seL4_VSpaceBits;
    default:
        return 0;
    }
}

int kobj_alloc(seL4_Word type, seL4_Word param, struct kobj *out)
{
    unsigned bits = object_bits(type, param);
    struct ut_block *block;
    uint64_t slot;

    if (bits == 0) {
        return -FASTIX_EINVAL;
    }
    int err = ut_buddy_alloc(&buddy, bits < UT_MIN_BITS ? UT_MIN_BITS : bits, &block);
    if (err != 0) {
        return err;
    }
    err = slot_alloc_get(&slots, &slot);
    if (err != 0) {
        goto out_block;
    }
    err = kobj_sel4_err(seL4_Untyped_Retype(block->slot, type, param, seL4_CapInitThreadCNode,
                                            0, 0, slot, 1));
    if (err != 0) {
        slot_alloc_put(&slots, slot);
        goto out_block;
    }
    out->block = block;
    out->cap = slot;
    return 0;

out_block:
    if (ut_buddy_free(&buddy, block) != 0) {
        libos_assert(0 && "cannot return untyped block");
    }
    return err;
}

void kobj_free(struct kobj *obj)
{
    /* Revoking the block deletes the object cap and everything derived from it. */
    if (ut_buddy_free(&buddy, obj->block) != 0) {
        libos_assert(0 && "cannot revoke untyped block");
    }
    slot_alloc_put(&slots, obj->cap);
    obj->block = NULL;
    obj->cap = seL4_CapNull;
}

int kobj_slot_get(seL4_CPtr *slot)
{
    uint64_t s;
    int err = slot_alloc_get(&slots, &s);

    if (err == 0) {
        *slot = s;
    }
    return err;
}

void kobj_slot_put(seL4_CPtr slot)
{
    slot_alloc_put(&slots, slot);
}

struct kobj_stats kobj_stats(void)
{
    return (struct kobj_stats){ buddy.free_bytes, buddy.blocks_in_use, slots.used };
}
