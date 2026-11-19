/* frame.c - pooled, reference-counted user frames and page mapping (see frame.h). */
#include <arch/vm.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <libos/assert.h>
#include <libos/mem.h>
#include <libos/slab.h>

#include "cspace.h"
#include "frame.h"
#include "heap.h"

#define SCRATCH_PAGES   2
#define FRAMES_PER_CHUNK (1u << (FASTIX_FRAME_CHUNK_BITS - FASTIX_PAGE_SHIFT))

static struct slab_cache frame_cache;
static struct list_node free_frames;
static uint64_t frames_total, frames_free;
static seL4_CPtr scratch_slot[SCRATCH_PAGES]; /* temporary copies of the frames being accessed */

int vm_map_cap(seL4_CPtr vspace, seL4_CPtr cap, uint64_t vaddr, bool writable, bool exec,
               pt_keep_fn keep, void *ctx)
{
    /* Each failed lookup means one missing page-table level; add it and retry. */
    for (int level = 0; level <= ARCH_PT_LEVELS; level++) {
        seL4_Error err = arch_map_page(cap, vspace, vaddr, writable, exec);
        if (err != seL4_FailedLookup) {
            return kobj_sel4_err(err);
        }
        struct kobj pt;
        int e = kobj_alloc(ARCH_PAGE_TABLE_OBJECT, 0, &pt);
        if (e != 0) {
            return e;
        }
        e = keep(ctx, &pt);
        if (e != 0) {
            kobj_free(&pt);
            return e;
        }
        e = kobj_sel4_err(arch_map_page_table(pt.cap, vspace, vaddr));
        if (e != 0) {
            return e; /* the owner frees the recorded table */
        }
    }
    return -FASTIX_EINVAL;
}

static void *scratch_addr(int i)
{
    return (void *)(uintptr_t)(FASTIX_PROCD_SCRATCH_VADDR + (uint64_t)i * FASTIX_PAGE_SIZE);
}

static void scratch_unmap(int i)
{
    /* Deleting the copy also removes its mapping. */
    if (seL4_CNode_Delete(cspace_root(), scratch_slot[i], seL4_WordBits) != seL4_NoError) {
        libos_assert(0 && "cannot delete scratch copy");
    }
}

/* Maps a temporary copy of `cap` at scratch page i (a frame cap maps only once). */
static int scratch_map(int i, seL4_CPtr cap)
{
    int err = kobj_sel4_err(seL4_CNode_Copy(cspace_root(), scratch_slot[i], seL4_WordBits,
                                            cspace_root(), cap, seL4_WordBits, seL4_AllRights));
    if (err != 0) {
        return err;
    }
    err = kobj_sel4_err(arch_map_page(scratch_slot[i], seL4_CapInitThreadVSpace,
                                      (uint64_t)(uintptr_t)scratch_addr(i), true, false));
    if (err != 0) {
        scratch_unmap(i);
    }
    return err;
}

/* Cuts one 2 MB untyped chunk into frames and adds them to the free list. */
static int grow(void)
{
    struct kobj chunk;
    int err = kobj_alloc(seL4_UntypedObject, FASTIX_FRAME_CHUNK_BITS, &chunk);
    if (err != 0) {
        return err; /* chunks are kept forever, so `chunk` is never freed */
    }
    for (unsigned i = 0; i < FRAMES_PER_CHUNK; i++) {
        struct frame *f = slab_alloc(&frame_cache);
        if (f == NULL) {
            return frames_free > 0 ? 0 : -FASTIX_ENOMEM;
        }
        err = kobj_slot_get(&f->cap);
        if (err == 0) {
            err = kobj_sel4_err(cspace_retype(chunk.cap, ARCH_FRAME_OBJECT, 0, f->cap));
            if (err != 0) {
                kobj_slot_put(f->cap);
            }
        }
        if (err != 0) {
            slab_free(&frame_cache, f);
            return frames_free > 0 ? 0 : err;
        }
        f->dirty = false; /* fresh frames are zeroed by the kernel */
        list_push(&free_frames, &f->link);
        frames_total++;
        frames_free++;
    }
    return 0;
}

int frame_init(void)
{
    slab_init(&frame_cache, "frame", sizeof(struct frame), heap_provider());
    list_init(&free_frames);
    for (int i = 0; i < SCRATCH_PAGES; i++) {
        int err = kobj_slot_get(&scratch_slot[i]);
        if (err != 0) {
            return err;
        }
    }
    /* Map once at both scratch pages so their page tables exist from now on. */
    struct frame *probe;
    int err = frame_new(&probe);
    for (int i = 0; err == 0 && i < SCRATCH_PAGES; i++) {
        err = kobj_sel4_err(seL4_CNode_Copy(cspace_root(), scratch_slot[i], seL4_WordBits,
                                            cspace_root(), probe->cap, seL4_WordBits,
                                            seL4_AllRights));
        if (err == 0) {
            err = vm_map_cap(seL4_CapInitThreadVSpace, scratch_slot[i],
                             (uint64_t)(uintptr_t)scratch_addr(i), true, false, heap_keep_pt, NULL);
            scratch_unmap(i);
        }
    }
    if (probe != NULL) {
        frame_put(probe);
    }
    return err;
}

int frame_new(struct frame **out)
{
    *out = NULL;
    if (list_empty(&free_frames)) {
        int err = grow();
        if (err != 0) {
            return err;
        }
    }
    struct frame *f = LIST_ENTRY(list_pop(&free_frames), struct frame, link);
    frames_free--;
    if (f->dirty) {
        /* Never hand one process's old data to another. */
        int err = scratch_map(0, f->cap);
        if (err != 0) {
            list_push(&free_frames, &f->link);
            frames_free++;
            return err;
        }
        memset(scratch_addr(0), 0, FASTIX_PAGE_SIZE);
        scratch_unmap(0);
        f->dirty = false;
    }
    f->refs = 1;
    *out = f;
    return 0;
}

void frame_get(struct frame *f)
{
    f->refs++;
}

void frame_put(struct frame *f)
{
    libos_assert(f->refs > 0);
    if (--f->refs != 0) {
        return;
    }
    /* Remove every copy of the cap (and so every mapping) that might remain. */
    if (seL4_CNode_Revoke(cspace_root(), f->cap, seL4_WordBits) != seL4_NoError) {
        libos_assert(0 && "cannot revoke frame");
    }
    f->dirty = true;
    list_push(&free_frames, &f->link);
    frames_free++;
}

int frame_write(struct frame *f, uint64_t offset, const void *src, size_t len, bool exec)
{
    if (offset > FASTIX_PAGE_SIZE || len > FASTIX_PAGE_SIZE - offset) {
        return -FASTIX_EINVAL;
    }
    int err = scratch_map(0, f->cap);
    if (err != 0) {
        return err;
    }
    memcpy((uint8_t *)scratch_addr(0) + offset, src, len);
    if (exec && len > 0) {
        err = kobj_sel4_err(arch_sync_icache(scratch_slot[0], offset, offset + len));
    }
    scratch_unmap(0);
    return err;
}

int frame_read(struct frame *f, uint64_t offset, void *dst, size_t len)
{
    if (offset > FASTIX_PAGE_SIZE || len > FASTIX_PAGE_SIZE - offset) {
        return -FASTIX_EINVAL;
    }
    int err = scratch_map(0, f->cap);
    if (err != 0) {
        return err;
    }
    memcpy(dst, (const uint8_t *)scratch_addr(0) + offset, len);
    scratch_unmap(0);
    return 0;
}

int frame_copy(struct frame *dst, struct frame *src)
{
    int err = scratch_map(0, src->cap);
    if (err != 0) {
        return err;
    }
    err = scratch_map(1, dst->cap);
    if (err == 0) {
        memcpy(scratch_addr(1), scratch_addr(0), FASTIX_PAGE_SIZE);
        scratch_unmap(1);
    }
    scratch_unmap(0);
    return err;
}

uint64_t frame_live(void)
{
    return frames_total - frames_free;
}
