/*
 * vmspace.h - a process address space: memory areas (VMAs), the pages
 * mapped in them, and the page tables holding those mappings.
 *
 * Copy-on-write needs no special flag: a page is mapped writable only when
 * its VMA allows writing AND no other address space shares its frame
 * (frame->refs == 1). A write to a shared page faults and gets a private copy.
 */
#ifndef PROCD_VMSPACE_H
#define PROCD_VMSPACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libos/compiler.h>
#include <libos/list.h>
#include <libos/rtree.h>

#include "frame.h"
#include "kobj.h"

#define VMA_SPECIAL 0x1u /* set up by procd (VDSO, info page, IPC buffer): fork skips it, users can't change it */
#define VMA_HEAP    0x2u /* part of the brk heap */

struct vma {
    struct rnode node; /* [start, end), page aligned */
    uint32_t prot;     /* FASTIX_PROT_* */
    uint32_t flags;    /* VMA_* */
};

struct page {
    struct rnode node;    /* one page */
    struct frame *frame;
    seL4_CPtr cap;        /* this address space's copy of the frame cap */
    bool mapped;          /* hardware mapping state, so changes are only made when needed */
    bool writable;
    bool exec;
};

struct vmspace {
    struct kobj root;     /* the seL4 VSpace object */
    struct rtree vmas;
    struct rtree pages;
    struct list_node pts; /* page tables (struct pt_rec) */
    uint64_t brk_start;   /* first byte of the brk heap */
    uint64_t brk_end;     /* current program break */
};

enum vm_access { VM_READ, VM_WRITE, VM_EXEC };

struct vm_stats {
    uint64_t vmas;  /* VMAs alive in all address spaces */
    uint64_t pages; /* page records alive */
    uint64_t pts;   /* page tables owned by address spaces */
};

void vms_module_init(void);
LIBOS_MUST_CHECK int vms_init(struct vmspace *vs);
void vms_destroy(struct vmspace *vs); /* frees every page, VMA, table and the VSpace */

/* mmap for anonymous private memory. Returns 0 and the address in *out, or -errno. */
LIBOS_MUST_CHECK int vms_mmap(struct vmspace *vs, uint64_t addr, uint64_t len, uint32_t prot,
                              uint32_t flags, uint64_t *out);
LIBOS_MUST_CHECK int vms_munmap(struct vmspace *vs, uint64_t addr, uint64_t len);
LIBOS_MUST_CHECK int vms_mprotect(struct vmspace *vs, uint64_t addr, uint64_t len, uint32_t prot);

/* brk: on success or failure *out is the (possibly unchanged) break; never returns an error. */
void vms_brk(struct vmspace *vs, uint64_t addr, uint64_t *out);

/*
 * Handles a page fault. Returns 0 if the access can now proceed, or 1 with the
 * signal to deliver in *sig and *code (SIGSEGV MAPERR/ACCERR, SIGBUS when out of memory).
 */
int vms_fault(struct vmspace *vs, uint64_t addr, enum vm_access access, int *sig, int *code);

/* Copies every non-special VMA of `src` into `dst`, sharing frames copy-on-write. */
LIBOS_MUST_CHECK int vms_clone(struct vmspace *dst, struct vmspace *src);

/* procd writes into process memory (ignores protection; used to load programs). */
LIBOS_MUST_CHECK int vms_write(struct vmspace *vs, uint64_t addr, const void *src, size_t len);

/* Maps frame `f` as a one-page special VMA at `addr` (takes its own reference). */
LIBOS_MUST_CHECK int vms_add_special(struct vmspace *vs, uint64_t addr, struct frame *f,
                                     uint32_t prot);

struct vm_stats vms_stats(void);

#endif /* PROCD_VMSPACE_H */
