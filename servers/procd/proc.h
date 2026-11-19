/*
 * proc.h - processes: table, spawn, fork, exit/kill, and wait.
 * A process is one thread (threads arrive in Phase 3) with its own address
 * space and a 64-slot CNode holding only its badged procd endpoint.
 * A dead process stays as a zombie (resources freed, exit status kept)
 * until its parent collects it with wait4.
 */
#ifndef PROCD_PROC_H
#define PROCD_PROC_H

#include <stdbool.h>
#include <stdint.h>

#include <libos/compiler.h>
#include <sel4/sel4.h>

#include "frame.h"
#include "kobj.h"
#include "loader.h"
#include "reply.h"
#include "vmspace.h"

struct proc {
    uint32_t pid;
    uint16_t gen;             /* badge generation: badges of an older pid user never match */
    uint32_t ppid;            /* 0 for PID 1 */
    bool zombie;
    int status;               /* wait status, once a zombie */
    struct vmspace vs;
    struct kobj cnode;
    struct kobj tcb;
    struct kobj sc;
    seL4_CPtr fault_ep;       /* badged fault endpoint copy in procd's CNode */
    struct frame *procinfo;
    struct frame *ipcbuf;
    struct reply_obj *wait_reply; /* parked wait4 call, or NULL */
    int32_t wait_pid;         /* what the parked wait4 waits for (-1 = any child) */
    uint64_t ipc_count;       /* calls received from this process (tests) */
};

void proc_module_init(seL4_CPtr endpoint, seL4_CPtr sched_control);

/* Live or zombie process with this pid, or NULL. */
struct proc *proc_get(uint32_t pid);

/* Live process a badge belongs to (pid and generation must match), or NULL. */
struct proc *proc_from_badge(uint64_t badge);

/* Creates a process running the given program. Returns 0 or -errno. */
LIBOS_MUST_CHECK int proc_spawn(const struct load_args *la, uint32_t ppid, struct proc **out);

/* Creates a copy-on-write copy of `parent`, which must be blocked in its fork call.
 * The child starts at once and sees fork() return 0. Returns 0 or -errno. */
LIBOS_MUST_CHECK int proc_fork(struct proc *parent, struct proc **out);

/* Ends a live process with wait status `status`: frees its resources, makes it a
 * zombie, hands its children to PID 1, and wakes a parent parked in wait4. */
void proc_exit(struct proc *p, int status);

/*
 * wait4 for `p`: pid -1 = any child, pid > 0 = that child.
 * Returns 0 with *child_pid and *status set (child_pid 0 if WNOHANG and none exited),
 * 1 if the caller must block, or -FASTIX_ECHILD / -FASTIX_EINVAL.
 */
int proc_wait(struct proc *p, int32_t pid, uint64_t options, uint32_t *child_pid, int *status);

/* Processes in the table (live + zombie). */
uint64_t proc_count(void);

#endif /* PROCD_PROC_H */
