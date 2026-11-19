/*
 * pipe.h - pipe2 (backed by libos SPSC rings) and eventfd2 in procd.
 *
 * TEMPORARY FD TABLE: Because vfsd does not exist yet (Phase 4), procd keeps a
 * minimal per-process FD table (PROCD_TEMP_MAX_FDS slots) solely for pipe and
 * eventfd descriptors. In Phase 4 (AGENTS.md 6.4), FD numbering and OFD tables
 * move to vfsd, and procd keeps only the pipe/eventfd objects themselves.
 */
#ifndef PROCD_PIPE_H
#define PROCD_PIPE_H

#include <stdbool.h>
#include <stdint.h>

#include "proc.h"

void pipe_module_init(void);

/* Process lifecycle hooks for the temporary FD table. */
void pipe_proc_init(struct proc *p, const struct proc *parent);
void pipe_proc_exec(struct proc *p);
void pipe_proc_cleanup(struct proc *p);

/* Cancels a thread parked in PARK_PIPE_* or PARK_EFD_*. */
void pipe_cancel_wait(struct thread *t);

/* Syscall handlers. */
int pipe_sys_pipe2(struct proc *p, uint64_t fds_u, uint32_t flags, int32_t out_fds[2]);
int pipe_sys_eventfd2(struct proc *p, uint32_t initval, uint32_t flags);
int pipe_sys_close(struct proc *p, int32_t fd);
int pipe_sys_dup(struct proc *p, int32_t oldfd);
int pipe_sys_dup3(struct proc *p, int32_t oldfd, int32_t newfd, uint32_t flags);
int pipe_sys_fcntl(struct proc *p, int32_t fd, uint32_t cmd, uint64_t arg);

/*
 * read / write on temporary pipe/eventfd FDs.
 * Sets *park_kind != PARK_NONE if caller must be parked; otherwise returns >= 0 or -errno.
 */
int64_t pipe_sys_read(struct thread *t, int32_t fd, uint64_t buf_u, uint64_t len,
                      enum park_kind *park_out);
int64_t pipe_sys_write(struct thread *t, int32_t fd, uint64_t buf_u, uint64_t len,
                       enum park_kind *park_out);

#endif /* PROCD_PIPE_H */
