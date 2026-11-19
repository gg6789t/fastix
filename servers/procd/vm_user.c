/* vm_user.c - procd reading and writing user memory (split from vmspace.c). */
#include <arch/vm.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <fastix/mman.h>
#include <fastix/signal_abi.h>
#include <fastix/vspace.h>
#include <libos/assert.h>
#include <libos/mem.h>
#include <libos/slab.h>
#include "cspace.h"
#include "heap.h"
#include "vm_internal.h"
#include "vmspace.h"

static int write_internal(struct vmspace *vs, uint64_t addr, const void *src, size_t len,
                          bool check_prot)
{
    const uint8_t *from = src;

    while (len > 0) {
        const struct vma *v = vm_vma_at(vs, addr);
        if (v == NULL) {
            return -FASTIX_EFAULT;
        }
        if (check_prot && !(v->prot & FASTIX_PROT_WRITE)) {
            return -FASTIX_EFAULT;
        }
        bool shared = (v->flags & VMA_SHARED) != 0;
        uint64_t page = addr & ~PAGE_MASK, off = addr & PAGE_MASK;
        size_t chunk = FASTIX_PAGE_SIZE - off < len ? (size_t)(FASTIX_PAGE_SIZE - off) : len;
        struct page *pg = vm_page_at(vs, page);
        int err = 0;
        if (pg == NULL) {
            err = vm_add_vma_page(vs, v, page);
        } else if (!shared && pg->frame->refs > 1) {
            err = vm_cow_copy(vs, pg, v->prot);
        }
        if (err == 0) {
            pg = vm_page_at(vs, page);
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

int vms_write(struct vmspace *vs, uint64_t addr, const void *src, size_t len)
{
    return write_internal(vs, addr, src, len, false);
}

int vms_write_user(struct vmspace *vs, uint64_t addr, const void *src, size_t len)
{
    return write_internal(vs, addr, src, len, true);
}

int vms_read(struct vmspace *vs, uint64_t addr, void *dst, size_t len)
{
    uint8_t *to = dst;

    while (len > 0) {
        const struct vma *v = vm_vma_at(vs, addr);
        if (v == NULL || v->prot == FASTIX_PROT_NONE) {
            return -FASTIX_EFAULT;
        }
        uint64_t page = addr & ~PAGE_MASK, off = addr & PAGE_MASK;
        size_t chunk = FASTIX_PAGE_SIZE - off < len ? (size_t)(FASTIX_PAGE_SIZE - off) : len;
        struct page *pg = vm_page_at(vs, page);
        if (pg == NULL) {
            /* Unfaulted private anonymous page reads as zero without allocating a frame. */
            if ((v->flags & VMA_SHARED) && v->shm != NULL) {
                struct rnode *sn = rtree_find(&v->shm->frames, v->shm_off + (page - v->node.start));
                if (sn != NULL) {
                    int err = frame_read(LIST_ENTRY(sn, struct shm_slot, node)->frame, off, to,
                                         chunk);
                    if (err != 0) {
                        return err;
                    }
                } else {
                    memset(to, 0, chunk);
                }
            } else {
                memset(to, 0, chunk);
            }
        } else {
            int err = frame_read(pg->frame, off, to, chunk);
            if (err != 0) {
                return err;
            }
        }
        addr += chunk;
        to += chunk;
        len -= chunk;
    }
    return 0;
}

int vms_read_str(struct vmspace *vs, uint64_t addr, char *dst, size_t max_len)
{
    if (max_len == 0) {
        return -FASTIX_EINVAL;
    }
    size_t pos = 0;
    while (pos < max_len) {
        uint64_t off = addr & PAGE_MASK;
        size_t chunk = FASTIX_PAGE_SIZE - off;
        if (chunk > max_len - pos) {
            chunk = max_len - pos;
        }
        int err = vms_read(vs, addr, dst + pos, chunk);
        if (err != 0) {
            return err;
        }
        for (size_t i = 0; i < chunk; i++) {
            if (dst[pos + i] == '\0') {
                return 0;
            }
        }
        pos += chunk;
        addr += chunk;
    }
    return -FASTIX_ENAMETOOLONG;
}

int vms_get_frame(struct vmspace *vs, uint64_t addr, bool for_write, struct frame **out,
                  bool *is_shared)
{
    const struct vma *v = vm_vma_at(vs, addr);
    if (v == NULL || v->prot == FASTIX_PROT_NONE) {
        return -FASTIX_EFAULT;
    }
    if (for_write && !(v->prot & FASTIX_PROT_WRITE)) {
        return -FASTIX_EFAULT;
    }
    bool shared = (v->flags & VMA_SHARED) != 0;
    uint64_t page = addr & ~PAGE_MASK;
    struct page *pg = vm_page_at(vs, page);
    int err = 0;
    if (pg == NULL) {
        err = vm_add_vma_page(vs, v, page);
    } else if (for_write && !shared && pg->frame->refs > 1) {
        err = vm_cow_copy(vs, pg, v->prot);
    }
    if (err != 0) {
        return err;
    }
    pg = vm_page_at(vs, page);
    *out = pg->frame;
    if (is_shared != NULL) {
        *is_shared = shared;
    }
    return 0;
}
