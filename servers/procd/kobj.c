/* kobj.c - procd kernel object allocator (see kobj.h). */
#include <fastix/config.h>
#include <fastix/errno.h>
#include <libos/kalloc.h>
#include <libos/slot_alloc.h>

#include "cspace.h"
#include "kobj.h"

static uint64_t slot_bits[SLOT_ALLOC_WORDS(FASTIX_PROCD_MAX_SLOTS)];
static struct slot_alloc slots;
static struct ut_block ut_nodes[FASTIX_PROCD_UT_NODES];
static struct kalloc ka;

int kobj_sel4_err(seL4_Error err)
{
    return kalloc_sel4_err(err);
}

int kobj_init(const seL4_BootInfo *bi, seL4_Word skip_untyped, seL4_CPtr first_slot,
              seL4_Word num_slots)
{
    if (num_slots > FASTIX_PROCD_MAX_SLOTS) {
        return -FASTIX_EINVAL;
    }
    slot_alloc_init(&slots, first_slot, num_slots, slot_bits);
    kalloc_init(&ka, ut_nodes, FASTIX_PROCD_UT_NODES, &slots, cspace_root(), cspace_retype);

    for (seL4_Word i = 0; i < bi->untyped.end - bi->untyped.start; i++) {
        const seL4_UntypedDesc *ut = &bi->untypedList[i];
        if (ut->isDevice || i == skip_untyped) {
            continue; /* device memory goes to devd; skip_untyped holds procd's CSpace */
        }
        int err = ut_buddy_add(&ka.buddy, bi->untyped.start + i, ut->paddr, ut->sizeBits);
        if (err != 0) {
            return err;
        }
    }
    return 0;
}

int kobj_alloc(seL4_Word type, seL4_Word param, struct kobj *out)
{
    return kalloc_obj(&ka, type, param, out);
}

void kobj_free(struct kobj *obj)
{
    kalloc_free(&ka, obj);
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
    return (struct kobj_stats){ ka.buddy.free_bytes, ka.buddy.blocks_in_use, slots.used };
}
