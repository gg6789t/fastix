/*
 * frame.h - physical pages of user memory, shared by reference count.
 * A frame's own cap is never mapped into a process; every mapping uses a copy
 * of it, so one frame can appear in several address spaces (copy-on-write
 * after fork, the shared VDSO page).
 *
 * Frames come from a pool: 2 MB untyped chunks are cut into 512 frames at
 * once (one allocator block per chunk, not per page). When a frame's last
 * reference goes, every copy of its cap is revoked and the frame returns to
 * the pool; it is zeroed before it is handed out again. Chunks are kept.
 */
#ifndef PROCD_FRAME_H
#define PROCD_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libos/compiler.h>
#include <libos/list.h>
#include <sel4/sel4.h>

#include "kobj.h"

struct frame {
    struct list_node link; /* on the free list while unused */
    seL4_CPtr cap;         /* original frame cap (copies are made for mappings) */
    uint32_t refs;         /* page records using it (+1 while its creator holds it) */
    bool dirty;            /* was used before: must be zeroed before reuse */
};

/* Creates the scratch window used to fill and copy frames. Call once at boot. */
LIBOS_MUST_CHECK int frame_init(void);

/* New zeroed frame with refs = 1 (the caller's reference). Returns 0 or -errno. */
LIBOS_MUST_CHECK int frame_new(struct frame **out);
void frame_get(struct frame *f);
void frame_put(struct frame *f); /* frees the frame when refs reaches 0 */

/* Writes `len` bytes at `offset`; with `exec`, also syncs the instruction cache. */
LIBOS_MUST_CHECK int frame_write(struct frame *f, uint64_t offset, const void *src, size_t len,
                                 bool exec);

/* Copies the whole page `src` into `dst`. */
LIBOS_MUST_CHECK int frame_copy(struct frame *dst, struct frame *src);

/* Frames in use right now (for leak checks). */
uint64_t frame_live(void);

/* Records a page table created by vm_map_cap; returns 0 or -errno. */
typedef int (*pt_keep_fn)(void *ctx, const struct kobj *pt);

/* Maps `cap` at `vaddr` in `vspace`, creating missing page tables (handed to `keep`).
 * Mapping an already-mapped cap at the same address changes its rights. */
LIBOS_MUST_CHECK int vm_map_cap(seL4_CPtr vspace, seL4_CPtr cap, uint64_t vaddr, bool writable,
                                bool exec, pt_keep_fn keep, void *ctx);

#endif /* PROCD_FRAME_H */
