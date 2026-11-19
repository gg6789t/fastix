/*
 * server_internal.h - what procd's server modules share: one decoded call, how it is
 * answered, Reply-object parking, and the handlers in server_vfs.c.
 */
#ifndef PROCD_SERVER_INTERNAL_H
#define PROCD_SERVER_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <sel4/sel4.h>

#include "proc.h"
#include "reply.h"

#define CALL_MAX_ARGS 6
#define CALL_MAX_RET  7

enum outcome {
    OUT_REPLY,  /* answer now with ret[0..nret-1] */
    OUT_PARKED, /* the caller waits; its Reply object was parked */
    OUT_NONE,   /* no answer: the caller exited or was restarted via WriteRegisters */
};

struct call {
    struct thread *t;
    struct proc *p;
    seL4_Word label;
    uint64_t arg[CALL_MAX_ARGS];
    uint64_t ret[CALL_MAX_RET];
    unsigned nret;
};

/* Takes the current Reply object for parking (a fresh one serves the loop). Returns 0/-errno. */
LIBOS_MUST_CHECK int take_reply(struct reply_obj **out);

/* Gives back a Reply object from take_reply when parking failed. */
void untake_reply(struct reply_obj *rep);

/* server_vfs.c: calls that wait for vfsd (fork, posix_spawn, pipe2, eventfd2) and pipe I/O. */
enum outcome park_for_vfs(struct call *c, int (*start)(struct call *c));
int start_fork(struct call *c);
int start_spawn(struct call *c);
int start_pipe2(struct call *c);
int start_eventfd2(struct call *c);
enum outcome sys_io_rw(struct call *c, bool is_write);

#endif /* PROCD_SERVER_INTERNAL_H */
