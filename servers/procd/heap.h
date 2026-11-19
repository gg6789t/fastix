/*
 * heap.h - procd's own memory: a heap of pages for slab caches, and the
 * page tables of procd's own mappings (heap, scratch window, VDSO view).
 * Heap pages are never returned (the slab caches keep them warm).
 */
#ifndef PROCD_HEAP_H
#define PROCD_HEAP_H

#include <libos/slab.h>

#include "kobj.h"

/* Page provider for slab caches. */
struct slab_provider heap_provider(void);

/* Records a page table created for one of procd's own mappings (vm_map_cap keep callback). */
int heap_keep_pt(void *ctx, const struct kobj *pt);

/* Pages the heap has taken so far. */
uint64_t heap_pages(void);

#endif /* PROCD_HEAP_H */
