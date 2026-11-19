/*
 * vfslink.h - procd's side of the vfsd link (docs/adr/0003-vfsd-core.md): starts vfsd, keeps
 * one vfsd client record per process, queues work items that vfsd pulls with downward
 * FASTIX_SRV_VFS_WORK calls, and reads the FD tables vfsd exports.
 */
#ifndef PROCD_VFSLINK_H
#define PROCD_VFSLINK_H

#include <stdbool.h>
#include <stdint.h>

#include <libos/compiler.h>
#include <sel4/sel4.h>

struct proc;
struct thread;

/* No vfsd client slot (struct proc.vfs_slot). */
#define VFSLINK_NO_SLOT 0xFFFFFFFFu

/* Creates vfsd's endpoint and wake-up notification (before any process exists). */
LIBOS_MUST_CHECK int vfslink_init(void);

/* Spawns vfsd from its embedded image. Processes created earlier wait until vfsd runs. */
LIBOS_MUST_CHECK int vfslink_start(void);

/*
 * Registers new process `p` with vfsd, copying the FDs of `parent` (NULL = empty table;
 * `cloexec`: leave out FD_CLOEXEC descriptors, as posix_spawn's exec does). p's main thread
 * starts with p->start_regs when vfsd is done. `waiter` (if non-NULL) must then be parked in
 * PARK_VFS; it gets MR0 = p->pid or -errno. Must be the last step that can fail.
 */
LIBOS_MUST_CHECK int vfslink_new(struct proc *p, struct proc *parent, struct thread *waiter,
                                 bool cloexec);

/* Asks vfsd to close p's close-on-exec FDs; p's main thread starts afterwards. */
LIBOS_MUST_CHECK int vfslink_exec(struct proc *p);

/* Points vfsd's view of p's I/O windows at p->iowin (a vfork child that called execve). */
LIBOS_MUST_CHECK int vfslink_set_window(struct proc *p);

/* Tells vfsd that `p` exited. Never fails (the item was reserved by vfslink_new). */
void vfslink_exit(struct proc *p);

/*
 * Asks vfsd to install `n` (1 or 2) procd objects as new FDs of t's process. `flags[i]` =
 * open flags | object kind << FASTIX_VFS_KIND_SHIFT. The caller must park `t` in PARK_VFS.
 * When done, t gets MR0 = fd0 (fds_u == 0) or 0 after {fd0, fd1} were stored at fds_u.
 * If installing fails, the objects are released and t gets -errno.
 */
LIBOS_MUST_CHECK int vfslink_install(struct thread *t, unsigned n, const uint32_t obj[2],
                                     const uint32_t flags[2], uint64_t fds_u);

/* Exported FD word of `fd` in p's table (owner << 28 | index), 0 if fd is not open. */
uint32_t vfslink_fd_word(const struct proc *p, int32_t fd);

/* Handles a FASTIX_BADGE_SERVER call from vfsd; reply words go to the IPC buffer. */
void vfslink_handle_server_msg(seL4_MessageInfo_t info, unsigned *nret);

#endif /* PROCD_VFSLINK_H */
