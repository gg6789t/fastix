/*
 * proc_internal.h - helpers shared by proc.c, thread.c and proc_ids.c only.
 * Not part of procd's public process API (proc.h).
 */
#ifndef PROCD_PROC_INTERNAL_H
#define PROCD_PROC_INTERNAL_H

#include <stdint.h>

#include <libos/compiler.h>
#include <sel4/sel4.h>

#include "proc.h"

/* Endpoint rights for clients: send, and receive a reply. */
#define CLIENT_EP_RIGHTS seL4_CapRights_new(1, 0, 0, 1) /* grant_reply, grant, read, write */
#define CRED_UNCHANGED   UINT32_MAX                     /* "-1": leave this id as it is */

/* procd's endpoint, minted with per-thread badges. */
seL4_CPtr proc_endpoint(void);

/* New zeroed thread record, or NULL. */
struct thread *proc_thread_alloc(void);

/* Frees every resource of `t` and its record. */
void proc_thread_free(struct thread *t);

/* Creates t's TCB, SchedContext and IPC buffer at `ipcbuf_vaddr` in `vs`. Returns 0 or -errno. */
LIBOS_MUST_CHECK int proc_build_thread_objs(struct proc *p, struct thread *t, struct vmspace *vs,
                                            uint64_t ipcbuf_vaddr);

#endif /* PROCD_PROC_INTERNAL_H */
