/* cspace.c - procd's two-level CSpace (see cspace.h). */
#include <stdint.h>

#include <fastix/config.h>
#include <fastix/errno.h>

#include "cspace.h"

static seL4_CPtr root = seL4_CapInitThreadCNode;
static unsigned l2_bits; /* 0 until the switch: then slots live in level-2 CNodes */

seL4_CPtr cspace_root(void)
{
    return root;
}

seL4_Error cspace_retype(seL4_CPtr untyped, seL4_Word type, seL4_Word size_bits, seL4_CPtr slot)
{
    if (l2_bits == 0) {
        return seL4_Untyped_Retype(untyped, type, size_bits, seL4_CapInitThreadCNode, 0, 0, slot,
                                   1);
    }
    /* Name the level-2 CNode holding `slot`: its CPtr is slot >> l2_bits, resolved
     * with depth WordBits - l2_bits (guard + level-1 index); then the slot offset. */
    return seL4_Untyped_Retype(untyped, type, size_bits, root, slot >> l2_bits,
                               seL4_WordBits - l2_bits, slot & ((1ULL << l2_bits) - 1), 1);
}

/* Smallest RAM untyped of at least `bytes`, or -1. */
static long pick_untyped(const seL4_BootInfo *bi, uint64_t bytes)
{
    long best = -1;
    for (seL4_Word i = 0; i < bi->untyped.end - bi->untyped.start; i++) {
        const seL4_UntypedDesc *ut = &bi->untypedList[i];
        if (!ut->isDevice && (1ULL << ut->sizeBits) >= bytes &&
            (best < 0 || ut->sizeBits < bi->untypedList[best].sizeBits)) {
            best = (long)i;
        }
    }
    return best;
}

int cspace_init(const seL4_BootInfo *bi, seL4_Word *untyped_index, seL4_CPtr *first_slot,
                seL4_Word *num_slots)
{
    const unsigned l1 = FASTIX_PROCD_CSPACE_L1_BITS, n = FASTIX_PROCD_CSPACE_L2_COUNT;
    const unsigned l2 = (unsigned)bi->initThreadCNodeSizeBits;

    if (n + 1 > (1u << l1) || ((uint64_t)n << l2) > FASTIX_PROCD_MAX_SLOTS ||
        bi->empty.end - bi->empty.start < n + 2) {
        return -FASTIX_EINVAL;
    }
    /* Level-2 CNodes first, then level 1, so each stays naturally aligned. */
    uint64_t bytes = ((uint64_t)n << (l2 + seL4_SlotBits)) + (1ULL << (l1 + seL4_SlotBits));
    long idx = pick_untyped(bi, bytes);
    if (idx < 0) {
        return -FASTIX_ENOMEM;
    }
    seL4_CPtr ut = bi->untyped.start + (seL4_Word)idx, cnodes = bi->empty.start, l1cap = cnodes + n;
    seL4_CPtr guarded = l1cap + 1; /* copy with the guard, so 64-bit CPtrs resolve through it */
    seL4_Word guard = seL4_CNode_CapData_new(0, seL4_WordBits - l1 - l2).words[0];
    seL4_Error err = seL4_Untyped_Retype(ut, seL4_CapTableObject, l2, seL4_CapInitThreadCNode, 0, 0,
                                         cnodes, n);
    if (err == seL4_NoError) {
        err = seL4_Untyped_Retype(ut, seL4_CapTableObject, l1, seL4_CapInitThreadCNode, 0, 0,
                                  l1cap, 1);
    }
    /* Entry 0: the boot CNode, with no guard so it resolves exactly l2 bits. */
    if (err == seL4_NoError) {
        err = seL4_CNode_Mint(l1cap, 0, l1, seL4_CapInitThreadCNode, seL4_CapInitThreadCNode,
                              seL4_WordBits, seL4_AllRights, seL4_CNode_CapData_new(0, 0).words[0]);
    }
    for (unsigned k = 0; err == seL4_NoError && k < n; k++) {
        err = seL4_CNode_Copy(l1cap, k + 1, l1, seL4_CapInitThreadCNode, cnodes + k, seL4_WordBits,
                              seL4_AllRights);
    }
    if (err == seL4_NoError) {
        err = seL4_CNode_Mint(seL4_CapInitThreadCNode, guarded, seL4_WordBits,
                              seL4_CapInitThreadCNode, l1cap, seL4_WordBits, seL4_AllRights, guard);
    }
    if (err == seL4_NoError) {
        err = seL4_TCB_SetSpace(seL4_CapInitThreadTCB, seL4_CapNull, l1cap, guard,
                                seL4_CapInitThreadVSpace, 0);
    }
    if (err != seL4_NoError) {
        return -FASTIX_EINVAL;
    }
    root = guarded; /* its CPtr still works: level-1 entry 0 is the boot CNode */
    l2_bits = l2;
    *untyped_index = (seL4_Word)idx;
    *first_slot = 1ULL << l2;
    *num_slots = (seL4_Word)n << l2;
    return 0;
}
