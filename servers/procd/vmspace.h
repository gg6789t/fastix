/*
 * vmspace.h - a process address space: memory areas (VMAs), the pages
 * mapped in them, and the page tables holding those mappings.
 *
 * Copy-on-write needs no special flag: a private page is mapped writable only
 * when its VMA allows writing AND no other address space shares its frame
 * (frame->refs == 1). A VMA_SHARED page stays writable even when shared.
 * Address spaces are reference-counted so vfork() can share one safely.
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

#define VMA_SPECIAL 0x1u /* set up by procd (VDSO, info page, IPC buffer): fork skips it */
#define VMA_HEAP    0x2u /* part of the brk heap */
#define VMA_SHARED  0x4u /* MAP_SHARED: stays shared and writable across fork */

struct anon_shm;

struct vma {
    struct rnode node;    /* [start, end), page aligned */
    uint32_t prot;        /* FASTIX_PROT_* */
    uint32_t flags;       /* VMA_* */
    struct anon_shm *shm; /* backing object when VMA_SHARED, else NULL */
    uint64_t shm_off;     /* byte offset into shm at node.start */
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
    uint32_t refs;        /* processes sharing this address space (normal = 1, vfork = 2) */
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

/* Allocates and initializes a new address space with refs = 1. Returns 0 or -errno. */
LIBOS_MUST_CHECK int vms_create(struct vmspace **out);
void vms_get(struct vmspace *vs);
void vms_put(struct vmspace *vs); /* destroys and frees vs when refs reaches 0 */

/* mmap for anonymous private or shared memory. Returns 0 and address in *out, or -errno. */
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

/* Copies every non-special VMA of `src` into `dst` (COW for private, shared for VMA_SHARED). */
LIBOS_MUST_CHECK int vms_clone(struct vmspace *dst, struct vmspace *src);

/* procd writes into process memory ignoring protection (used by ELF loader). */
LIBOS_MUST_CHECK int vms_write(struct vmspace *vs, uint64_t addr, const void *src, size_t len);

/* Writes into user memory checking VMA write permission (returns -EFAULT if not writable). */
LIBOS_MUST_CHECK int vms_write_user(struct vmspace *vs, uint64_t addr, const void *src, size_t len);

/* Reads from user memory checking VMA read permission (returns -EFAULT if not readable). */
LIBOS_MUST_CHECK int vms_read(struct vmspace *vs, uint64_t addr, void *dst, size_t len);

/* Reads a NUL-terminated string up to max_len bytes (including NUL). Returns 0 or -EFAULT/-ENAMETOOLONG. */
LIBOS_MUST_CHECK int vms_read_str(struct vmspace *vs, uint64_t addr, char *dst, size_t max_len);

/* Resolves `addr` to its backing physical frame (demand-allocating if valid). Returns 0 or -EFAULT/-ENOMEM. */
LIBOS_MUST_CHECK int vms_get_frame(struct vmspace *vs, uint64_t addr, bool for_write,
                                   struct frame **out, bool *is_shared);

/* Maps frame `f` as a one-page special VMA at `addr` (takes its own reference). */
LIBOS_MUST_CHECK int vms_add_special(struct vmspace *vs, uint64_t addr, struct frame *f,
                                     uint32_t prot);

/* Removes a one-page special VMA previously installed by vms_add_special. */
void vms_remove_special(struct vmspace *vs, uint64_t addr);

/* Maps `nframes` frames as a contiguous VMA_SHARED region in the mmap area. */
LIBOS_MUST_CHECK int vms_map_shared_frames(struct vmspace *vs, struct frame *const *frames,
                                           size_t nframes, uint32_t prot, uint64_t *out);

struct vm_stats vms_stats(void);

#endif /* PROCD_VMSPACE_H */
