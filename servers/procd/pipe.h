/*
 * pipe.h - pipe2 (libos SPSC rings) and eventfd2 in procd. vfsd owns the FD numbers
 * (AGENTS.md 6.4, docs/adr/0003-vfsd-core.md); procd owns one numbered object per open file
 * description and serves read/write on it by reading vfsd's exported FD table.
 */
#ifndef PROCD_PIPE_H
#define PROCD_PIPE_H

#include <stdbool.h>
#include <stdint.h>

#include "proc.h"

void pipe_module_init(void);

/* Cancels a thread parked in PARK_PIPE_* or PARK_EFD_*. */
void pipe_cancel_wait(struct thread *t);

/* pipe2 / eventfd2: on 0 the caller must park `t` in PARK_VFS (vfslink answers it). */
LIBOS_MUST_CHECK int pipe_sys_pipe2(struct thread *t, uint64_t fds_u, uint32_t flags);
LIBOS_MUST_CHECK int pipe_sys_eventfd2(struct thread *t, uint32_t initval, uint32_t flags);

/*
 * read / write on a pipe or eventfd FD. Sets *park_out != PARK_NONE if the caller must be
 * parked; otherwise returns >= 0 or -errno. -FASTIX_ESTALEFD means another server owns the
 * FD (*owner says which).
 */
int64_t pipe_sys_read(struct thread *t, int32_t fd, uint64_t buf_u, uint64_t len,
                      enum park_kind *park_out, uint32_t *owner);
int64_t pipe_sys_write(struct thread *t, int32_t fd, uint64_t buf_u, uint64_t len,
                       enum park_kind *park_out, uint32_t *owner);

/* Called by vfsd (through vfslink): the last OFD reference to object `index` is gone. */
void pipe_obj_release(uint32_t index);

/* Called by vfsd (through vfslink): F_SETFL changed object `index`'s O_NONBLOCK. */
void pipe_obj_setfl(uint32_t index, uint32_t flags);

/* Pipe-end and eventfd objects alive (tests: 0 leaked objects). */
uint64_t pipe_obj_count(void);

#endif /* PROCD_PIPE_H */
