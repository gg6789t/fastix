/*
 * vfsd.h - helpers shared by vfsd's server modules: client memory through the I/O window,
 * calls down to procd, and the address of each client's pages (docs/adr/0003-vfsd-core.md).
 */
#ifndef VFSD_VFSD_H
#define VFSD_VFSD_H

#include <stddef.h>
#include <stdint.h>

#include <fastix/vfs_proto.h>

/* vfsd's view of client slot `slot` at offset `off` (FASTIX_VFSD_*_OFF). */
static inline uint64_t client_view(uint32_t slot, uint64_t off)
{
    return fastix_vfsd_client_addr(slot, off);
}

/* vfsd address of [uaddr, uaddr + len) in the client's I/O window, or NULL if that range is
 * not entirely inside the window (the call then fails with EFAULT). */
void *uptr(uint32_t slot, uint64_t uaddr, size_t len);

/* Copies a NUL-terminated client string into dst (cap bytes). Returns 0, -EFAULT, or
 * -ENAMETOOLONG if it does not fit. */
int ustr(uint32_t slot, uint64_t uaddr, char *dst, size_t cap);

/* Synchronous downward call to procd's server-control endpoint; returns MR0. */
int64_t procd_call(uint64_t label, unsigned n, const uint64_t *mr);

/* Fetches and runs every queued procd work item (NEW, EXEC, EXIT, INSTALL). */
void work_drain(void);

#endif /* VFSD_VFSD_H */
