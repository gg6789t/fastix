/*
 * fastix/vfs_proto.h - vfsd protocols (AGENTS.md 6.3 C, 6.4; docs/adr/0003-vfsd-core.md):
 * the exported FD table read by procd, the procd -> vfsd work queue, and vfsd's address
 * layout for client pages.
 *
 * Client calls: label = FASTIX_SYS_* number, MR0..MR5 = the Linux-style arguments, reply
 * MR0 = result or -errno. Every pointer argument must point into the caller's own I/O window
 * (FASTIX_IOWIN_BASE ..), else -EFAULT. A call on an FD that another server owns returns
 * -FASTIX_ESTALEFD with MR1 = the owner (FASTIX_OWNER_*); the caller retries there once.
 */
#ifndef FASTIX_VFS_PROTO_H
#define FASTIX_VFS_PROTO_H

#include <stdint.h>

#include <fastix/config.h>
#include <fastix/vspace.h>

/* Who owns the object behind an FD. */
#define FASTIX_OWNER_NONE   0u
#define FASTIX_OWNER_VFSD   1u
#define FASTIX_OWNER_PROCD  2u

/* Kinds of procd objects (work item flags). */
#define FASTIX_OBJ_PIPE     1u
#define FASTIX_OBJ_EVENTFD  2u

/* Exported FD table: one 32-bit word per FD (owner << 28 | object index; 0 = closed),
 * written by vfsd with single atomic stores and mapped read-only in procd. */
#define FASTIX_FDEXP_OWNER_SHIFT 28
#define FASTIX_FDEXP_INDEX_MASK  0x0FFFFFFFu
#define FASTIX_FDEXP_PAGES \
    ((FASTIX_DEFAULT_NOFILE * sizeof(uint32_t) + FASTIX_PAGE_SIZE - 1u) / FASTIX_PAGE_SIZE)

static inline uint32_t fastix_fdexp_obj(uint32_t owner, uint32_t index)
{
    return (owner << FASTIX_FDEXP_OWNER_SHIFT) | (index & FASTIX_FDEXP_INDEX_MASK);
}

/* vfsd -> procd server calls (FASTIX_BADGE_SERVER, srvid FASTIX_SRVID_VFSD; rank 30 -> 10). */
#define FASTIX_SRV_VFS_WORK    0xE10 /* MR0 token done (0 = none), MR1 result, MR2 fd0, MR3 fd1
                                      * -> MR0 next token (0 = queue empty), MR1.. item */
#define FASTIX_SRV_OBJ_RELEASE 0xE11 /* MR0 object index: last OFD reference is gone */
#define FASTIX_SRV_OBJ_SETFL   0xE12 /* MR0 object index, MR1 status flags (O_NONBLOCK) */

/* Work items (reply of FASTIX_SRV_VFS_WORK): MR1 kind, MR2 client slot, MR3 pid, then
 *   NEW:     MR4 parent slot (or NO_PARENT), MR5 1 = leave out FD_CLOEXEC descriptors
 *   INSTALL: MR4/MR5 object index 0/1 (0 = none), MR6/MR7 their open flags with the
 *            object kind in bits 28..31. */
#define FASTIX_VFSW_NEW        1u /* new client; copy parent's FDs unless parent = NO_PARENT */
#define FASTIX_VFSW_EXEC       2u /* close FD_CLOEXEC descriptors */
#define FASTIX_VFSW_EXIT       3u /* close everything and forget the client */
#define FASTIX_VFSW_INSTALL    4u /* allocate FDs for 1 or 2 procd objects -> fd0, fd1 */
#define FASTIX_VFS_NO_PARENT   0xFFFFFFFFu
#define FASTIX_VFS_KIND_SHIFT  28

/* Test-only client call (debug/test builds): drains procd's work queue, then answers
 * MR1 clients, MR2 OFDs, MR3 tmpfs nodes, MR4 tmpfs data pages. */
#define FASTIX_VFS_TEST_STATS  0xF20

/* Client slots: one per live (or exiting) process known to vfsd. */
#define FASTIX_VFS_MAX_CLIENTS FASTIX_MAX_PROCS

/* vfsd's view of client slot s: I/O windows, then the procinfo page, then the FD export. */
#define FASTIX_VFSD_CLIENT_BASE   0x0000200000000000ULL
#define FASTIX_VFSD_CLIENT_STRIDE 0x0000000008000000ULL /* 128 MB */
#define FASTIX_VFSD_WIN_OFF       0ULL
#define FASTIX_VFSD_INFO_OFF      FASTIX_IOWIN_REGION
#define FASTIX_VFSD_FDEXP_OFF     (FASTIX_IOWIN_REGION + 0x10000ULL)

static inline uint64_t fastix_vfsd_client_addr(uint32_t slot, uint64_t off)
{
    return FASTIX_VFSD_CLIENT_BASE + (uint64_t)slot * FASTIX_VFSD_CLIENT_STRIDE + off;
}

_Static_assert(FASTIX_VFSD_FDEXP_OFF + FASTIX_FDEXP_PAGES * FASTIX_PAGE_SIZE <=
               FASTIX_VFSD_CLIENT_STRIDE, "vfsd client pages exceed the client stride");
_Static_assert(FASTIX_VSPACE_USER_END <= FASTIX_VFSD_CLIENT_BASE,
               "vfsd client pages overlap the normal process layout");
_Static_assert(FASTIX_VFSD_CLIENT_BASE + (unsigned long long)FASTIX_VFS_MAX_CLIENTS *
               FASTIX_VFSD_CLIENT_STRIDE <= 0x0000800000000000ULL, "vfsd client pages exceed 47-bit VA");

#endif /* FASTIX_VFS_PROTO_H */
