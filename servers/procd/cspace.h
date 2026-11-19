/*
 * cspace.h - procd's own capability space.
 * seL4 gives the root task one CNode of 2^12 slots, far too few. At boot
 * procd switches to a two-level CSpace: level-1 entry 0 is the original
 * CNode (so every boot CPtr keeps its value) and entries 1..N are new
 * CNodes of the same size, giving slots [2^12, (N+1) * 2^12).
 */
#ifndef PROCD_CSPACE_H
#define PROCD_CSPACE_H

#include <libos/compiler.h>
#include <sel4/sel4.h>

/*
 * Builds and switches to the bigger CSpace using one RAM untyped (returned in
 * *untyped_index; do not give it to the allocator). The new free slot range is
 * [*first_slot, *first_slot + *num_slots). Returns 0 or -errno.
 */
LIBOS_MUST_CHECK int cspace_init(const seL4_BootInfo *bi, seL4_Word *untyped_index,
                                 seL4_CPtr *first_slot, seL4_Word *num_slots);

/* CNode cap to use as the root for CNode operations with depth seL4_WordBits. */
seL4_CPtr cspace_root(void);

/* Retypes one object from `untyped` into procd slot `slot`. Returns a seL4 error. */
seL4_Error cspace_retype(seL4_CPtr untyped, seL4_Word type, seL4_Word size_bits, seL4_CPtr slot);

#endif /* PROCD_CSPACE_H */
