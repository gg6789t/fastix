/*
 * vm_internal.h - types and helpers shared by vmspace.c and vm_user.c only.
 * Not part of procd's public address-space API (vmspace.h).
 */
#ifndef PROCD_VM_INTERNAL_H
#define PROCD_VM_INTERNAL_H

#include <stdint.h>

#include <fastix/config.h>
#include <libos/compiler.h>
#include <libos/rtree.h>

#include "frame.h"
#include "vmspace.h"

#define PAGE_MASK ((uint64_t)FASTIX_PAGE_SIZE - 1)

/* One frame of a MAP_SHARED backing object. */
struct shm_slot {
    struct rnode node; /* [off, off + PAGE_SIZE) */
    struct frame *frame;
};

/* Backing object of MAP_SHARED anonymous memory, shared by every VMA mapping it. */
struct anon_shm {
    uint32_t refs;
    struct rtree frames; /* offset -> struct shm_slot */
};

/* VMA or page containing `addr`, or NULL. */
struct vma *vm_vma_at(const struct vmspace *vs, uint64_t addr);
struct page *vm_page_at(const struct vmspace *vs, uint64_t addr);

/* Adds a fresh (or shared-object) page for `page_addr` inside VMA `v`. Returns 0 or -errno. */
LIBOS_MUST_CHECK int vm_add_vma_page(struct vmspace *vs, const struct vma *v, uint64_t page_addr);

/* Gives `pg` a private copy of its shared frame. Returns 0 or -errno. */
LIBOS_MUST_CHECK int vm_cow_copy(struct vmspace *vs, struct page *pg, uint32_t prot);

#endif /* PROCD_VM_INTERNAL_H */
