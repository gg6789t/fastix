/*
 * vm.h - mapping frames into address spaces, and the list of kernel objects
 * a process owns (so destroying it frees every one of them).
 */
#ifndef PROCD_VM_H
#define PROCD_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <fastix/config.h>
#include <libos/compiler.h>
#include <sel4/sel4.h>

#include "kobj.h"

struct objlist {
    struct kobj obj[FASTIX_PROCD_MAX_PROC_OBJS];
    unsigned n;
};

/* Creates an object, records it in `list`, and returns its cap. Returns 0 or -errno. */
LIBOS_MUST_CHECK int objlist_alloc(struct objlist *list, seL4_Word type, seL4_Word param,
                                   seL4_CPtr *cap);

/* Frees every object in the list, newest first. */
void objlist_free_all(struct objlist *list);

/* Prepares procd's scratch window (allocates its page tables once). */
LIBOS_MUST_CHECK int vm_init(void);

/* Maps `frame` at `vaddr`, creating missing page tables (recorded in `owner`). */
LIBOS_MUST_CHECK int vm_map(struct objlist *owner, seL4_CPtr vspace, seL4_CPtr frame,
                            uint64_t vaddr, bool writable, bool exec);

/* Copies `len` bytes into `frame` at `offset` through procd's scratch window.
 * With `exec`, also makes the bytes visible to instruction fetch. */
LIBOS_MUST_CHECK int vm_write_frame(seL4_CPtr frame, uint64_t offset, const void *src,
                                    size_t len, bool exec);

#endif /* PROCD_VM_H */
