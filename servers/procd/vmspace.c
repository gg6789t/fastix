/* vmspace.c - process address spaces (see vmspace.h). */
#include <arch/vm.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <fastix/mman.h>
#include <fastix/signal_abi.h>
#include <fastix/vspace.h>
#include <libos/assert.h>
#include <libos/slab.h>

#include "cspace.h"
#include "heap.h"
#include "vmspace.h"

#define PAGE_MASK ((uint64_t)FASTIX_PAGE_SIZE - 1)

struct pt_rec {
    struct list_node link;
    struct kobj obj;
};

static struct slab_cache vma_cache, page_cache, pt_cache;

void vms_module_init(void)
{
    slab_init(&vma_cache, "vma", sizeof(struct vma), heap_provider());
    slab_init(&page_cache, "page", sizeof(struct page), heap_provider());
    slab_init(&pt_cache, "pt", sizeof(struct pt_rec), heap_provider());
}

struct vm_stats vms_stats(void)
{
    return (struct vm_stats){ vma_cache.in_use, page_cache.in_use, pt_cache.in_use };
}

/* ---- small helpers ---- */

static struct vma *vma_at(const struct vmspace *vs, uint64_t addr)
{
    struct rnode *n = rtree_find(&vs->vmas, addr);
    return n ? LIST_ENTRY(n, struct vma, node) : NULL;
}

static struct page *page_at(const struct vmspace *vs, uint64_t addr)
{
    struct rnode *n = rtree_find(&vs->pages, addr);
    return n ? LIST_ENTRY(n, struct page, node) : NULL;
}

/* Rounds len up to whole pages; 0 on overflow. */
static uint64_t page_round(uint64_t len)
{
    return len > UINT64_MAX - PAGE_MASK ? 0 : (len + PAGE_MASK) & ~PAGE_MASK;
}

/* Checks a page-aligned range lies inside the user address space. */
static bool user_range_ok(uint64_t addr, uint64_t size)
{
    return (addr & PAGE_MASK) == 0 && size != 0 && addr >= FASTIX_NULL_GUARD_END &&
           size <= FASTIX_VSPACE_USER_END - addr && addr < FASTIX_VSPACE_USER_END;
}

static bool overlaps_special(const struct vmspace *vs, uint64_t start, uint64_t end)
{
    for (struct rnode *n = rtree_next(&vs->vmas, start); n != NULL && n->start < end;
         n = rtree_next(&vs->vmas, n->end)) {
        if (LIST_ENTRY(n, struct vma, node)->flags & VMA_SPECIAL) {
            return true;
        }
    }
    return false;
}

static int keep_pt(void *ctx, const struct kobj *pt)
{
    struct vmspace *vs = ctx;
    struct pt_rec *r = slab_alloc(&pt_cache);

    if (r == NULL) {
        return -FASTIX_ENOMEM;
    }
    r->obj = *pt;
    list_push(&vs->pts, &r->link);
    return 0;
}

/* ---- pages ---- */

/* Makes the hardware mapping of `pg` match `prot` and the sharing state. */
static int page_apply(struct vmspace *vs, struct page *pg, uint32_t prot, bool *changed)
{
    bool writable = (prot & FASTIX_PROT_WRITE) && pg->frame->refs == 1;
    bool exec = (prot & FASTIX_PROT_EXEC) != 0;

    *changed = false;
    if (prot == FASTIX_PROT_NONE) {
        if (pg->mapped) {
            if (arch_unmap_page(pg->cap) != seL4_NoError) {
                libos_assert(0 && "cannot unmap page");
            }
            pg->mapped = false;
            *changed = true;
        }
        return 0;
    }
    if (pg->mapped && pg->writable == writable && pg->exec == exec) {
        return 0;
    }
    int err = vm_map_cap(vs->root.cap, pg->cap, pg->node.start, writable, exec, keep_pt, vs);
    if (err != 0) {
        return err;
    }
    pg->mapped = true;
    pg->writable = writable;
    pg->exec = exec;
    *changed = true;
    return 0;
}

static void page_remove(struct vmspace *vs, struct page *pg)
{
    rtree_remove(&vs->pages, &pg->node);
    /* Deleting our cap copy also removes the mapping. */
    if (seL4_CNode_Delete(cspace_root(), pg->cap, seL4_WordBits) != seL4_NoError) {
        libos_assert(0 && "cannot delete page cap");
    }
    kobj_slot_put(pg->cap);
    frame_put(pg->frame);
    slab_free(&page_cache, pg);
}

/* Adds a page record for frame `f` at `addr` (taking a reference) and maps it. */
static int page_add(struct vmspace *vs, uint64_t addr, struct frame *f, uint32_t prot)
{
    struct page *pg = slab_alloc(&page_cache);
    bool changed;

    if (pg == NULL) {
        return -FASTIX_ENOMEM;
    }
    int err = kobj_slot_get(&pg->cap);
    if (err != 0) {
        slab_free(&page_cache, pg);
        return err;
    }
    err = kobj_sel4_err(seL4_CNode_Copy(cspace_root(), pg->cap, seL4_WordBits,
                                        cspace_root(), f->cap, seL4_WordBits,
                                        seL4_AllRights));
    if (err != 0) {
        kobj_slot_put(pg->cap);
        slab_free(&page_cache, pg);
        return err;
    }
    frame_get(f);
    pg->frame = f;
    pg->node.start = addr;
    pg->node.end = addr + FASTIX_PAGE_SIZE;
    if (rtree_insert(&vs->pages, &pg->node) != 0) {
        libos_assert(0 && "page already present");
    }
    err = page_apply(vs, pg, prot, &changed);
    if (err != 0) {
        page_remove(vs, pg);
    }
    return err;
}

static int add_fresh_page(struct vmspace *vs, uint64_t addr, uint32_t prot)
{
    struct frame *f;
    int err = frame_new(&f);

    if (err != 0) {
        return err;
    }
    err = page_add(vs, addr, f, prot);
    frame_put(f); /* the page record holds its own reference */
    return err;
}

/* Gives `pg` a private copy of its shared frame. */
static int cow_copy(struct vmspace *vs, struct page *pg, uint32_t prot)
{
    struct frame *f;
    uint64_t addr = pg->node.start;
    int err = frame_new(&f);

    if (err != 0) {
        return err;
    }
    err = frame_copy(f, pg->frame);
    if (err == 0) {
        page_remove(vs, pg);
        err = page_add(vs, addr, f, prot);
    }
    frame_put(f);
    return err;
}

static void pages_remove_range(struct vmspace *vs, uint64_t start, uint64_t end)
{
    struct rnode *n = rtree_next(&vs->pages, start);

    while (n != NULL && n->start < end) {
        struct rnode *next = rtree_next(&vs->pages, n->end);
        page_remove(vs, LIST_ENTRY(n, struct page, node));
        n = next;
    }
}

/* ---- VMAs ---- */

static int vma_new(struct vmspace *vs, uint64_t start, uint64_t end, uint32_t prot,
                   uint32_t flags)
{
    struct vma *v = slab_alloc(&vma_cache);

    if (v == NULL) {
        return -FASTIX_ENOMEM;
    }
    v->node.start = start;
    v->node.end = end;
    v->prot = prot;
    v->flags = flags;
    int err = rtree_insert(&vs->vmas, &v->node);
    if (err != 0) {
        slab_free(&vma_cache, v);
    }
    return err;
}

/* Makes `at` a VMA boundary by splitting the VMA that straddles it. */
static int split_at(struct vmspace *vs, uint64_t at)
{
    struct vma *v = vma_at(vs, at);

    if (v == NULL || v->node.start == at) {
        return 0;
    }
    struct vma *upper = slab_alloc(&vma_cache);
    if (upper == NULL) {
        return -FASTIX_ENOMEM;
    }
    *upper = (struct vma){ .node = { .start = at, .end = v->node.end }, .prot = v->prot,
                           .flags = v->flags };
    v->node.end = at; /* shrinking in place keeps the tree ordered */
    if (rtree_insert(&vs->vmas, &upper->node) != 0) {
        libos_assert(0 && "split produced an overlap");
    }
    return 0;
}

static void vmas_remove_range(struct vmspace *vs, uint64_t start, uint64_t end)
{
    struct rnode *n = rtree_next(&vs->vmas, start);

    while (n != NULL && n->start < end) {
        struct rnode *next = rtree_next(&vs->vmas, n->end);
        rtree_remove(&vs->vmas, n);
        slab_free(&vma_cache, LIST_ENTRY(n, struct vma, node));
        n = next;
    }
}

/* Lowest free gap of `size` bytes in the mmap region, or 0 if none. */
static uint64_t find_free(const struct vmspace *vs, uint64_t size)
{
    uint64_t candidate = FASTIX_MMAP_BASE;

    while (size <= FASTIX_MMAP_END - candidate) {
        const struct rnode *n = rtree_next(&vs->vmas, candidate);
        if (n == NULL || n->start >= candidate + size) {
            return candidate;
        }
        candidate = n->end;
    }
    return 0;
}

static bool range_is_free(const struct vmspace *vs, uint64_t start, uint64_t end)
{
    const struct rnode *n = rtree_next(&vs->vmas, start);
    return n == NULL || n->start >= end;
}

/* ---- public operations ---- */

int vms_init(struct vmspace *vs)
{
    rtree_init(&vs->vmas);
    rtree_init(&vs->pages);
    list_init(&vs->pts);
    vs->brk_start = 0;
    vs->brk_end = 0;
    int err = kobj_alloc(ARCH_VSPACE_OBJECT, 0, &vs->root);
    if (err != 0) {
        vs->root.block = NULL;
        return err;
    }
    err = kobj_sel4_err(arch_assign_asid(vs->root.cap));
    if (err != 0) {
        kobj_free(&vs->root);
    }
    return err;
}

void vms_destroy(struct vmspace *vs)
{
    pages_remove_range(vs, 0, UINT64_MAX);
    vmas_remove_range(vs, 0, UINT64_MAX);
    struct list_node *n;
    while ((n = list_pop(&vs->pts)) != NULL) {
        struct pt_rec *r = LIST_ENTRY(n, struct pt_rec, link);
        kobj_free(&r->obj);
        slab_free(&pt_cache, r);
    }
    if (vs->root.block != NULL) {
        kobj_free(&vs->root);
    }
}

int vms_mmap(struct vmspace *vs, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags,
             uint64_t *out)
{
    uint64_t size = page_round(len);

    if (size == 0 || (prot & ~(uint32_t)FASTIX_PROT_MASK) != 0) {
        return -FASTIX_EINVAL;
    }
    if ((prot & FASTIX_PROT_WRITE) && (prot & FASTIX_PROT_EXEC)) {
        return -FASTIX_EACCES; /* W^X */
    }
    if ((flags & FASTIX_MAP_TYPE) != FASTIX_MAP_PRIVATE) {
        return -FASTIX_EINVAL; /* MAP_SHARED arrives with shared memory objects */
    }
    if (!(flags & FASTIX_MAP_ANONYMOUS)) {
        return -FASTIX_EBADF; /* file mappings arrive with vfsd */
    }
    if (flags & (FASTIX_MAP_FIXED | FASTIX_MAP_FIXED_NOREPLACE)) {
        if ((addr & PAGE_MASK) != 0) {
            return -FASTIX_EINVAL;
        }
        if (!user_range_ok(addr, size)) {
            return -FASTIX_ENOMEM;
        }
        if (overlaps_special(vs, addr, addr + size)) {
            return -FASTIX_EINVAL;
        }
        if (!range_is_free(vs, addr, addr + size)) {
            if (flags & FASTIX_MAP_FIXED_NOREPLACE) {
                return -FASTIX_EEXIST;
            }
            int err = vms_munmap(vs, addr, size);
            if (err != 0) {
                return err;
            }
        }
    } else {
        bool hint_ok = addr >= FASTIX_MMAP_BASE && (addr & PAGE_MASK) == 0 &&
                       size <= FASTIX_MMAP_END - addr && addr < FASTIX_MMAP_END &&
                       range_is_free(vs, addr, addr + size);
        addr = hint_ok ? addr : find_free(vs, size);
        if (addr == 0) {
            return -FASTIX_ENOMEM;
        }
    }
    int err = vma_new(vs, addr, addr + size, prot, 0);
    if (err == 0) {
        *out = addr;
    }
    return err;
}

int vms_munmap(struct vmspace *vs, uint64_t addr, uint64_t len)
{
    uint64_t size = page_round(len);

    if ((addr & PAGE_MASK) != 0 || size == 0 || size > UINT64_MAX - addr) {
        return -FASTIX_EINVAL;
    }
    uint64_t end = addr + size;
    if (overlaps_special(vs, addr, end)) {
        return -FASTIX_EINVAL;
    }
    int err = split_at(vs, addr);
    if (err == 0) {
        err = split_at(vs, end);
    }
    if (err != 0) {
        return err;
    }
    vmas_remove_range(vs, addr, end);
    pages_remove_range(vs, addr, end);
    return 0;
}

int vms_mprotect(struct vmspace *vs, uint64_t addr, uint64_t len, uint32_t prot)
{
    uint64_t size = page_round(len);

    if ((addr & PAGE_MASK) != 0 || size == 0 || size > UINT64_MAX - addr ||
        (prot & ~(uint32_t)FASTIX_PROT_MASK) != 0) {
        return -FASTIX_EINVAL;
    }
    if ((prot & FASTIX_PROT_WRITE) && (prot & FASTIX_PROT_EXEC)) {
        return -FASTIX_EACCES; /* W^X */
    }
    uint64_t end = addr + size;
    if (overlaps_special(vs, addr, end)) {
        return -FASTIX_EINVAL;
    }
    /* POSIX: the whole range must be mapped. */
    for (uint64_t at = addr; at < end;) {
        const struct rnode *n = rtree_find(&vs->vmas, at);
        if (n == NULL) {
            return -FASTIX_ENOMEM;
        }
        at = n->end;
    }
    int err = split_at(vs, addr);
    if (err == 0) {
        err = split_at(vs, end);
    }
    if (err != 0) {
        return err;
    }
    for (struct rnode *n = rtree_next(&vs->vmas, addr); n != NULL && n->start < end;
         n = rtree_next(&vs->vmas, n->end)) {
        LIST_ENTRY(n, struct vma, node)->prot = prot;
    }
    for (struct rnode *n = rtree_next(&vs->pages, addr); n != NULL && n->start < end;
         n = rtree_next(&vs->pages, n->end)) {
        bool changed;
        err = page_apply(vs, LIST_ENTRY(n, struct page, node), prot, &changed);
        if (err != 0) {
            return err;
        }
    }
    return 0;
}

void vms_brk(struct vmspace *vs, uint64_t addr, uint64_t *out)
{
    *out = vs->brk_end;
    if (addr < vs->brk_start || addr > FASTIX_MMAP_BASE) {
        return; /* includes brk(0), the "what is the break" query */
    }
    uint64_t old_top = page_round(vs->brk_end), new_top = page_round(addr);
    if (new_top > old_top) {
        if (!range_is_free(vs, old_top, new_top) ||
            vma_new(vs, old_top, new_top, FASTIX_PROT_READ | FASTIX_PROT_WRITE, VMA_HEAP) != 0) {
            return;
        }
    } else if (new_top < old_top && vms_munmap(vs, new_top, old_top - new_top) != 0) {
        return;
    }
    vs->brk_end = addr;
    *out = addr;
}

static bool access_allowed(uint32_t prot, enum vm_access access)
{
    switch (access) {
    case VM_WRITE:
        return (prot & FASTIX_PROT_WRITE) != 0;
    case VM_EXEC:
        return (prot & FASTIX_PROT_EXEC) != 0;
    default:
        return prot != FASTIX_PROT_NONE;
    }
}

int vms_fault(struct vmspace *vs, uint64_t addr, enum vm_access access, int *sig, int *code)
{
    const struct vma *v = vma_at(vs, addr);

    *sig = FASTIX_SIGSEGV;
    *code = FASTIX_SEGV_MAPERR;
    if (v == NULL) {
        return 1;
    }
    *code = FASTIX_SEGV_ACCERR;
    if (!access_allowed(v->prot, access)) {
        return 1;
    }
    struct page *pg = page_at(vs, addr & ~PAGE_MASK);
    bool changed = true;
    int err;
    if (pg == NULL) {
        err = add_fresh_page(vs, addr & ~PAGE_MASK, v->prot); /* demand-zero */
    } else if (access == VM_WRITE && pg->frame->refs > 1) {
        err = cow_copy(vs, pg, v->prot);
    } else {
        err = page_apply(vs, pg, v->prot, &changed); /* e.g. sole owner again after a fork */
    }
    if (err != 0) {
        *sig = FASTIX_SIGBUS;
        *code = FASTIX_BUS_ADRERR;
        return 1;
    }
    return changed ? 0 : 1; /* nothing to fix means the access really is not allowed */
}

int vms_clone(struct vmspace *dst, struct vmspace *src)
{
    for (struct rnode *n = rtree_next(&src->vmas, 0); n != NULL;
         n = rtree_next(&src->vmas, n->end)) {
        const struct vma *v = LIST_ENTRY(n, struct vma, node);
        if (v->flags & VMA_SPECIAL) {
            continue;
        }
        int err = vma_new(dst, n->start, n->end, v->prot, v->flags);
        if (err != 0) {
            return err;
        }
        for (struct rnode *p = rtree_next(&src->pages, n->start); p != NULL && p->start < n->end;
             p = rtree_next(&src->pages, p->end)) {
            struct page *pg = LIST_ENTRY(p, struct page, node);
            bool changed;
            err = page_add(dst, p->start, pg->frame, v->prot);
            if (err == 0) {
                err = page_apply(src, pg, v->prot, &changed); /* now shared: parent goes read-only */
            }
            if (err != 0) {
                return err;
            }
        }
    }
    dst->brk_start = src->brk_start;
    dst->brk_end = src->brk_end;
    return 0;
}

int vms_write(struct vmspace *vs, uint64_t addr, const void *src, size_t len)
{
    const uint8_t *from = src;

    while (len > 0) {
        const struct vma *v = vma_at(vs, addr);
        if (v == NULL) {
            return -FASTIX_EFAULT;
        }
        uint64_t page = addr & ~PAGE_MASK, off = addr & PAGE_MASK;
        size_t chunk = FASTIX_PAGE_SIZE - off < len ? (size_t)(FASTIX_PAGE_SIZE - off) : len;
        struct page *pg = page_at(vs, page);
        int err = 0;
        if (pg == NULL) {
            err = add_fresh_page(vs, page, v->prot);
        } else if (pg->frame->refs > 1) {
            err = cow_copy(vs, pg, v->prot);
        }
        if (err == 0) {
            pg = page_at(vs, page);
            err = frame_write(pg->frame, off, from, chunk, (v->prot & FASTIX_PROT_EXEC) != 0);
        }
        if (err != 0) {
            return err;
        }
        addr += chunk;
        from += chunk;
        len -= chunk;
    }
    return 0;
}

int vms_add_special(struct vmspace *vs, uint64_t addr, struct frame *f, uint32_t prot)
{
    int err = vma_new(vs, addr, addr + FASTIX_PAGE_SIZE, prot, VMA_SPECIAL);
    if (err != 0) {
        return err;
    }
    err = page_add(vs, addr, f, prot);
    if (err != 0) {
        vmas_remove_range(vs, addr, addr + FASTIX_PAGE_SIZE);
    }
    return err;
}
