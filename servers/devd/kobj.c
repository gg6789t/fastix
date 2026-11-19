/*
 * servers/devd/kobj.c - devd RAM object allocator, CSpace slot allocator,
 * binary-splitting Device Untyped MMIO frame carver, and scratch mapping.
 */
#include <arch/vm.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <libdriver/mmio.h>
#include <libos/assert.h>
#include <libos/kalloc.h>
#include <libos/slot_alloc.h>

#include "devd_internal.h"

#define DEVD_TOTAL_SLOTS   ((size_t)1U << FASTIX_DEVD_CSPACE_BITS)
#define DEVD_FREE_SLOTS    (DEVD_TOTAL_SLOTS - DEVD_CAP_FREE_FIRST)
#define DEVD_DEV_POOL_MAX  128
#define DEVD_MMIO_CACHE    16
#define DEVD_OWN_PT_MAX    8

struct dev_ut_entry {
    uint64_t paddr;
    uint32_t bits;
    seL4_CPtr cap;
};

struct mmio_cache_entry {
    uint64_t paddr;
    seL4_CPtr cap;
};

static uint64_t slot_bits[SLOT_ALLOC_WORDS(DEVD_FREE_SLOTS)];
static struct slot_alloc slots;
static struct ut_block ut_nodes[FASTIX_DEVD_UT_NODES];
static struct kalloc ka;

static struct dev_ut_entry dev_pool[DEVD_DEV_POOL_MAX];
static size_t dev_pool_len;
static struct mmio_cache_entry mmio_cache[DEVD_MMIO_CACHE];
static size_t mmio_cache_len;

static struct kobj own_pts[DEVD_OWN_PT_MAX];
static size_t own_pt_len;

static seL4_Error retype_one(seL4_CPtr parent, seL4_Word type, seL4_Word param, seL4_CPtr slot)
{
    return seL4_Untyped_Retype(parent, type, param, DEVD_CAP_SELF_CNODE, 0, 0, slot, 1);
}

int devd_kobj_init(const struct devd_bootinfo *bi)
{
    slot_alloc_init(&slots, DEVD_CAP_FREE_FIRST, DEVD_FREE_SLOTS, slot_bits);
    kalloc_init(&ka, ut_nodes, FASTIX_DEVD_UT_NODES, &slots, DEVD_CAP_SELF_CNODE, retype_one);

    int err = ut_buddy_add(&ka.buddy, DEVD_CAP_RAM_UT, bi->ram_ut_paddr, bi->ram_ut_bits);
    if (err != 0) {
        return err;
    }

    dev_pool_len = 0;
    for (uint32_t i = 0; i < bi->dev_ut_count && i < FASTIX_DEVD_MAX_DEV_UTS; i++) {
        if (dev_pool_len >= DEVD_DEV_POOL_MAX) {
            return -FASTIX_ENOMEM;
        }
        dev_pool[dev_pool_len++] = (struct dev_ut_entry){
            .paddr = bi->dev_uts[i].paddr,
            .bits = bi->dev_uts[i].size_bits,
            .cap = bi->dev_uts[i].slot,
        };
    }
    return 0;
}

int dkobj_alloc(seL4_Word type, seL4_Word param, struct kobj *out)
{
    return kalloc_obj(&ka, type, param, out);
}

void dkobj_free(struct kobj *obj)
{
    kalloc_free(&ka, obj);
}

int dslot_get(seL4_CPtr *slot)
{
    uint64_t s = 0;
    int err = slot_alloc_get(&slots, &s);
    if (err == 0) {
        *slot = s;
    }
    return err;
}

void dslot_put(seL4_CPtr slot)
{
    if (slot != seL4_CapNull) {
        slot_alloc_put(&slots, slot);
    }
}

int dslot_delete_and_put(seL4_CPtr slot)
{
    if (slot == seL4_CapNull) {
        return 0;
    }
    seL4_Error err = seL4_CNode_Delete(DEVD_CAP_SELF_CNODE, slot, seL4_WordBits);
    slot_alloc_put(&slots, slot);
    return kalloc_sel4_err(err);
}

/* Carves a 4 KB device frame at exact physical address page_paddr by binary-splitting
 * the containing Device Untyped; caches carved frames for reuse on shared pages/restarts. */
int devd_mmio_frame(uint64_t page_paddr, seL4_CPtr *cap_out)
{
    if ((page_paddr & (FASTIX_PAGE_SIZE - 1U)) != 0) {
        return -FASTIX_EINVAL;
    }
    for (size_t i = 0; i < mmio_cache_len; i++) {
        if (mmio_cache[i].paddr == page_paddr) {
            *cap_out = mmio_cache[i].cap;
            return 0;
        }
    }

    size_t idx = dev_pool_len;
    for (size_t i = 0; i < dev_pool_len; i++) {
        uint64_t size = 1ULL << dev_pool[i].bits;
        if (page_paddr >= dev_pool[i].paddr && page_paddr < dev_pool[i].paddr + size) {
            idx = i;
            break;
        }
    }
    if (idx == dev_pool_len) {
        return -FASTIX_ENODEV;
    }

    while (dev_pool[idx].bits > seL4_PageBits) {
        if (dev_pool_len >= DEVD_DEV_POOL_MAX) {
            return -FASTIX_ENOMEM;
        }
        seL4_CPtr s0 = 0, s1 = 0;
        int err = dslot_get(&s0);
        if (err != 0) {
            return err;
        }
        err = dslot_get(&s1);
        if (err != 0) {
            dslot_put(s0);
            return err;
        }
        uint32_t child_bits = dev_pool[idx].bits - 1U;
        uint64_t base = dev_pool[idx].paddr;
        uint64_t half = 1ULL << child_bits;
        seL4_CPtr parent = dev_pool[idx].cap;

        seL4_Error se = retype_one(parent, seL4_UntypedObject, child_bits, s0);
        if (se == seL4_NoError) {
            se = retype_one(parent, seL4_UntypedObject, child_bits, s1);
        }
        if (se != seL4_NoError) {
            return kalloc_sel4_err(se);
        }

        if (page_paddr < base + half) {
            dev_pool[idx] = (struct dev_ut_entry){ base, child_bits, s0 };
            dev_pool[dev_pool_len++] = (struct dev_ut_entry){ base + half, child_bits, s1 };
        } else {
            dev_pool[idx] = (struct dev_ut_entry){ base + half, child_bits, s1 };
            dev_pool[dev_pool_len++] = (struct dev_ut_entry){ base, child_bits, s0 };
        }
    }

    if (mmio_cache_len >= DEVD_MMIO_CACHE) {
        return -FASTIX_ENOMEM;
    }
    seL4_CPtr frame_cap = 0;
    int err = dslot_get(&frame_cap);
    if (err != 0) {
        return err;
    }
    seL4_Error se = retype_one(dev_pool[idx].cap, ARCH_FRAME_OBJECT, 0, frame_cap);
    if (se != seL4_NoError) {
        dslot_put(frame_cap);
        return kalloc_sel4_err(se);
    }

    dev_pool[idx] = dev_pool[--dev_pool_len];
    mmio_cache[mmio_cache_len++] = (struct mmio_cache_entry){ page_paddr, frame_cap };
    *cap_out = frame_cap;
    return 0;
}

int devd_map_scratch(seL4_CPtr frame, bool is_mmio)
{
    for (int lvl = 0; lvl <= ARCH_PT_LEVELS; lvl++) {
        seL4_Error err = is_mmio
            ? arch_map_mmio_page(frame, DEVD_CAP_SELF_VSPACE, DEVD_VADDR_SCRATCH)
            : arch_map_page(frame, DEVD_CAP_SELF_VSPACE, DEVD_VADDR_SCRATCH, true, false);
        if (err == seL4_NoError) {
            return 0;
        }
        if (err != seL4_FailedLookup || lvl == ARCH_PT_LEVELS || own_pt_len >= DEVD_OWN_PT_MAX) {
            return kalloc_sel4_err(err);
        }
        struct kobj pt;
        int aerr = dkobj_alloc(ARCH_PAGE_TABLE_OBJECT, 0, &pt);
        if (aerr != 0) {
            return aerr;
        }
        err = arch_map_page_table(pt.cap, DEVD_CAP_SELF_VSPACE, DEVD_VADDR_SCRATCH);
        if (err != seL4_NoError) {
            dkobj_free(&pt);
            return kalloc_sel4_err(err);
        }
        own_pts[own_pt_len++] = pt;
    }
    return -FASTIX_EINVAL;
}

void devd_unmap_scratch(seL4_CPtr frame)
{
    (void)arch_unmap_page(frame);
}

void devd_kobj_stats(uint64_t *free_bytes, uint64_t *slots_used)
{
    if (free_bytes != NULL) {
        *free_bytes = ka.buddy.free_bytes;
    }
    if (slots_used != NULL) {
        *slots_used = slots.used;
    }
}
