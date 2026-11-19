/* server_vfs.c - procd calls that wait for vfsd, and read/write on pipe and eventfd FDs. */
#include <stdbool.h>
#include <stdint.h>

#include <fastix/errno.h>

#include "exec.h"
#include "pipe.h"
#include "proc.h"
#include "server_internal.h"

/* Runs `start` (fork, posix_spawn, pipe2, eventfd2); on 0 parks the caller until vfsd has
 * finished, and vfslink answers it. */
enum outcome park_for_vfs(struct call *c, int (*start)(struct call *c))
{
    struct reply_obj *rep;
    int err = take_reply(&rep);
    if (err == 0) {
        err = start(c);
        if (err == 0) {
            c->t->park_reply = rep;
            c->t->park_kind = PARK_VFS;
            return OUT_PARKED;
        }
        untake_reply(rep);
    }
    c->ret[0] = (uint64_t)(int64_t)err;
    return OUT_REPLY;
}

int start_fork(struct call *c)
{
    struct proc *child;
    return proc_fork(c->t, &child);
}

int start_spawn(struct call *c)
{
    return proc_posix_spawn(c->t, c->arg[0]);
}

int start_pipe2(struct call *c)
{
    return pipe_sys_pipe2(c->t, c->arg[0], (uint32_t)c->arg[1]);
}

int start_eventfd2(struct call *c)
{
    return pipe_sys_eventfd2(c->t, (uint32_t)c->arg[0], (uint32_t)c->arg[1]);
}

/* read/write on pipe and eventfd FDs; other FDs answer -ESTALEFD with MR1 = their owner. */
enum outcome sys_io_rw(struct call *c, bool is_write)
{
    enum park_kind pk = PARK_NONE;
    uint32_t owner = 0;
    int32_t fd = (int32_t)c->arg[0];
    int64_t r = is_write ? pipe_sys_write(c->t, fd, c->arg[1], c->arg[2], &pk, &owner)
                         : pipe_sys_read(c->t, fd, c->arg[1], c->arg[2], &pk, &owner);
    if (pk != PARK_NONE) {
        struct reply_obj *rep;
        int err = take_reply(&rep);
        c->t->park_kind = pk;
        if (err == 0) {
            c->t->park_reply = rep;
            return OUT_PARKED;
        }
        pipe_cancel_wait(c->t); /* needs park_kind set to find the wait list */
        c->t->park_kind = PARK_NONE;
        r = err;
    }
    c->ret[0] = (uint64_t)r;
    c->ret[1] = owner;
    c->nret = r == -FASTIX_ESTALEFD ? 2 : 1;
    return OUT_REPLY;
}
