/* heap.c - procd heap pages and procd's own page tables (see heap.h). */
#include <arch/vm.h>
#include <fastix/config.h>
#include <fastix/errno.h>

#include "frame.h"
#include "heap.h"

static struct kobj own_pts[FASTIX_PROCD_OWN_PTS];
static unsigned n_own_pts;
static uint64_t heap_next = FASTIX_PROCD_HEAP_VADDR;

int heap_keep_pt(void *ctx, const struct kobj *pt)
{
    (void)ctx;
    if (n_own_pts == FASTIX_PROCD_OWN_PTS) {
        return -FASTIX_ENOMEM;
    }
    own_pts[n_own_pts++] = *pt;
    return 0;
}

static int heap_get_page(void *ctx, void **page)
{
    struct kobj frame;

    (void)ctx;
    if (heap_next - FASTIX_PROCD_HEAP_VADDR >= FASTIX_PROCD_HEAP_MAX) {
        return -FASTIX_ENOMEM;
    }
    int err = kobj_alloc(ARCH_FRAME_OBJECT, 0, &frame);
    if (err != 0) {
        return err;
    }
    err = vm_map_cap(seL4_CapInitThreadVSpace, frame.cap, heap_next, true, false, heap_keep_pt,
                     NULL);
    if (err != 0) {
        kobj_free(&frame);
        return err;
    }
    *page = (void *)(uintptr_t)heap_next; /* fresh frames are zeroed by the kernel */
    heap_next += FASTIX_PAGE_SIZE;
    return 0;
}

struct slab_provider heap_provider(void)
{
    return (struct slab_provider){ heap_get_page, NULL };
}

uint64_t heap_pages(void)
{
    return (heap_next - FASTIX_PROCD_HEAP_VADDR) / FASTIX_PAGE_SIZE;
}
