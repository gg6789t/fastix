/* kalloc.c - kernel object allocator shared by servers (see libos/kalloc.h). */
#include <arch/vm.h>
#include <fastix/errno.h>
#include <libos/assert.h>
#include <libos/kalloc.h>

int kalloc_sel4_err(seL4_Error err)
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
    struct kalloc *ka = ctx;
    return kalloc_sel4_err(ka->retype(parent, seL4_UntypedObject, bits, slot));
}

static int backend_revoke(void *ctx, uint64_t parent)
{
    struct kalloc *ka = ctx;
    return kalloc_sel4_err(seL4_CNode_Revoke(ka->root, parent, seL4_WordBits));
}

void kalloc_init(struct kalloc *ka, struct ut_block *nodes, size_t n, struct slot_alloc *slots,
                 seL4_CPtr root, kalloc_retype_fn retype)
{
    ka->slots = slots;
    ka->root = root;
    ka->retype = retype;
    ut_buddy_init(&ka->buddy, nodes, n, slots,
                  (struct ut_backend){ backend_split, backend_revoke, ka });
}

unsigned kalloc_object_bits(seL4_Word type, seL4_Word param)
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

int kalloc_obj(struct kalloc *ka, seL4_Word type, seL4_Word param, struct kobj *out)
{
    unsigned bits = kalloc_object_bits(type, param);
    struct ut_block *block;
    uint64_t slot;

    if (bits == 0) {
        return -FASTIX_EINVAL;
    }
    int err = ut_buddy_alloc(&ka->buddy, bits < UT_MIN_BITS ? UT_MIN_BITS : bits, &block);
    if (err != 0) {
        return err;
    }
    err = slot_alloc_get(ka->slots, &slot);
    if (err != 0) {
        goto out_block;
    }
    err = kalloc_sel4_err(ka->retype(block->slot, type, param, slot));
    if (err != 0) {
        slot_alloc_put(ka->slots, slot);
        goto out_block;
    }
    out->block = block;
    out->cap = slot;
    return 0;

out_block:
    if (ut_buddy_free(&ka->buddy, block) != 0) {
        libos_assert(0 && "cannot return untyped block");
    }
    return err;
}

void kalloc_free(struct kalloc *ka, struct kobj *obj)
{
    if (obj->block == NULL) {
        return;
    }
    /* Revoking the block deletes the object cap and everything derived from it. */
    if (ut_buddy_free(&ka->buddy, obj->block) != 0) {
        libos_assert(0 && "cannot revoke untyped block");
    }
    slot_alloc_put(ka->slots, obj->cap);
    obj->block = NULL;
    obj->cap = seL4_CapNull;
}
