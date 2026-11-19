/* proc.c - process table, spawn, fork, exit and wait (see proc.h). */
#include <arch/counter.h>
#include <arch/regs.h>
#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/cspace.h>
#include <fastix/errno.h>
#include <fastix/mman.h>
#include <fastix/vdso.h>
#include <fastix/vspace.h>
#include <libos/assert.h>
#include <libos/slab.h>

#include "cspace.h"
#include "heap.h"
#include "proc.h"
#include "vdso.h"

/* Endpoint rights for clients: send, and receive a reply. */
#define CLIENT_EP_RIGHTS seL4_CapRights_new(1, 0, 0, 1) /* grant_reply, grant, read, write */
#define DEFAULT_UMASK    022

static struct slab_cache proc_cache;
static struct proc *table[FASTIX_MAX_PROCS]; /* slot = pid % FASTIX_MAX_PROCS */
static uint64_t n_procs;
static uint32_t last_pid;
static uint16_t next_gen = 1;
static seL4_CPtr endpoint;
static seL4_CPtr sched_control;

void proc_module_init(seL4_CPtr ep, seL4_CPtr sc_control)
{
    slab_init(&proc_cache, "proc", sizeof(struct proc), heap_provider());
    endpoint = ep;
    sched_control = sc_control;
}

uint64_t proc_count(void)
{
    return n_procs;
}

struct proc *proc_get(uint32_t pid)
{
    struct proc *p = table[pid % FASTIX_MAX_PROCS];
    return (p != NULL && p->pid == pid) ? p : NULL;
}

struct proc *proc_from_badge(uint64_t badge)
{
    struct proc *p = proc_get(fastix_badge_pid(badge));
    return (p != NULL && !p->zombie && p->gen == fastix_badge_gen(badge)) ? p : NULL;
}

/* Picks the next pid whose table slot is free. */
static int pid_alloc(uint32_t *pid)
{
    for (uint32_t tries = 0; tries < FASTIX_PID_MAX; tries++) {
        last_pid = last_pid % FASTIX_PID_MAX + 1;
        if (table[last_pid % FASTIX_MAX_PROCS] == NULL) {
            *pid = last_pid;
            return 0;
        }
    }
    return -FASTIX_EAGAIN;
}

static int proc_alloc(uint32_t ppid, struct proc **out)
{
    uint32_t pid;
    int err = pid_alloc(&pid);
    if (err != 0) {
        return err;
    }
    struct proc *p = slab_alloc(&proc_cache); /* zeroed: no resources yet */
    if (p == NULL) {
        return -FASTIX_ENOMEM;
    }
    p->pid = pid;
    p->gen = next_gen++;
    p->ppid = ppid;
    table[pid % FASTIX_MAX_PROCS] = p;
    n_procs++;
    *out = p;
    return 0;
}

static void proc_free(struct proc *p)
{
    table[p->pid % FASTIX_MAX_PROCS] = NULL;
    n_procs--;
    slab_free(&proc_cache, p);
}

/* Frees every resource of `p` (safe on a partly built process). */
static void release(struct proc *p)
{
    if (p->tcb.block != NULL) {
        (void)seL4_TCB_Suspend(p->tcb.cap); /* stop it before its memory goes away */
        kobj_free(&p->tcb);
    }
    if (p->wait_reply != NULL) {
        reply_put(p->wait_reply); /* the caller it was bound to no longer exists */
        p->wait_reply = NULL;
    }
    if (p->sc.block != NULL) {
        kobj_free(&p->sc);
    }
    if (p->fault_ep != seL4_CapNull) {
        if (seL4_CNode_Delete(cspace_root(), p->fault_ep, seL4_WordBits) !=
            seL4_NoError) {
            libos_assert(0 && "cannot delete fault endpoint copy");
        }
        kobj_slot_put(p->fault_ep);
        p->fault_ep = seL4_CapNull;
    }
    vms_destroy(&p->vs);
    if (p->cnode.block != NULL) {
        kobj_free(&p->cnode);
    }
    if (p->procinfo != NULL) {
        frame_put(p->procinfo);
        p->procinfo = NULL;
    }
    if (p->ipcbuf != NULL) {
        frame_put(p->ipcbuf);
        p->ipcbuf = NULL;
    }
}

/* Step 1: address space, CSpace, thread, scheduling context, and the one endpoint cap. */
static int build_caps(struct proc *p)
{
    int err;

    if ((err = vms_init(&p->vs)) != 0 ||
        (err = kobj_alloc(seL4_CapTableObject, FASTIX_ROOT_CNODE_BITS, &p->cnode)) != 0 ||
        (err = kobj_alloc(seL4_TCBObject, 0, &p->tcb)) != 0 ||
        (err = kobj_alloc(seL4_SchedContextObject, seL4_MinSchedContextBits, &p->sc)) != 0) {
        return err;
    }
    uint64_t badge = fastix_badge_make(FASTIX_BADGE_SYSCALL, p->pid, 0, p->gen);
    return kobj_sel4_err(seL4_CNode_Mint(p->cnode.cap, FASTIX_CAP_PROCD_EP_MAIN,
                                         FASTIX_ROOT_CNODE_BITS, cspace_root(), endpoint,
                                         seL4_WordBits, CLIENT_EP_RIGHTS, badge));
}

/* Step 2: the pages procd provides: VDSO clock, info page, IPC buffer. */
static int build_special_pages(struct proc *p)
{
    struct fastix_procinfo info = {
        .pid = p->pid, .ppid = p->ppid, .pgid = p->pid, .sid = p->pid, .umask = DEFAULT_UMASK,
    };
    int err;

    if ((err = vms_add_special(&p->vs, FASTIX_VDSO_ADDR, vdso_frame(), FASTIX_PROT_READ)) != 0 ||
        (err = frame_new(&p->procinfo)) != 0 ||
        (err = frame_write(p->procinfo, 0, &info, sizeof(info), false)) != 0 ||
        (err = vms_add_special(&p->vs, FASTIX_PROCINFO_ADDR, p->procinfo, FASTIX_PROT_READ)) != 0 ||
        (err = frame_new(&p->ipcbuf)) != 0) {
        return err;
    }
    return vms_add_special(&p->vs, FASTIX_IPCBUF_BASE, p->ipcbuf,
                           FASTIX_PROT_READ | FASTIX_PROT_WRITE);
}

/* Step 3: configure the thread, give it CPU time and a fault handler (not started yet). */
static int build_thread(struct proc *p)
{
    /* CSpace guard: a 64-slot root CNode resolves full 64-bit CPtrs. */
    seL4_Word guard = seL4_CNode_CapData_new(0, seL4_WordBits - FASTIX_ROOT_CNODE_BITS).words[0];
    int err = kobj_sel4_err(seL4_TCB_Configure(p->tcb.cap, p->cnode.cap, guard, p->vs.root.cap, 0,
                                               FASTIX_IPCBUF_BASE, p->ipcbuf->cap));
    if (err != 0 ||
        (err = kobj_sel4_err(seL4_SchedControl_ConfigureFlags(
             sched_control, p->sc.cap, FASTIX_DEFAULT_TIMESLICE_US, FASTIX_DEFAULT_TIMESLICE_US,
             0, 0, 0))) != 0) {
        return err;
    }
    if ((err = kobj_slot_get(&p->fault_ep)) != 0) {
        p->fault_ep = seL4_CapNull;
        return err;
    }
    uint64_t badge = fastix_badge_make(FASTIX_BADGE_FAULT, p->pid, 0, p->gen);
    err = kobj_sel4_err(seL4_CNode_Mint(cspace_root(), p->fault_ep, seL4_WordBits,
                                        cspace_root(), endpoint, seL4_WordBits,
                                        CLIENT_EP_RIGHTS, badge));
    if (err != 0) {
        kobj_slot_put(p->fault_ep);
        p->fault_ep = seL4_CapNull;
        return err;
    }
    return kobj_sel4_err(seL4_TCB_SetSchedParams(p->tcb.cap, seL4_CapInitThreadTCB, 0,
                                                 FASTIX_DEFAULT_PRIO, p->sc.cap, p->fault_ep));
}

static int build(struct proc *p)
{
    int err = build_caps(p);
    if (err == 0) {
        err = build_special_pages(p);
    }
    if (err == 0) {
        err = build_thread(p);
    }
    return err;
}

static int start(struct proc *p, seL4_UserContext *regs)
{
    return kobj_sel4_err(seL4_TCB_WriteRegisters(p->tcb.cap, 1, 0, ARCH_REGS_COUNT, regs));
}

static void discard(struct proc *p)
{
    release(p);
    proc_free(p);
}

int proc_spawn(const struct load_args *la, uint32_t ppid, struct proc **out)
{
    struct proc *p;
    uint64_t entry, sp;
    seL4_UserContext regs;

    int err = proc_alloc(ppid, &p);
    if (err != 0) {
        return err;
    }
    if ((err = build(p)) == 0 && (err = loader_load(&p->vs, la, &entry, &sp)) == 0) {
        arch_regs_init_entry(&regs, entry, sp, 0);
        err = start(p, &regs);
    }
    if (err != 0) {
        discard(p);
        return err;
    }
    *out = p;
    return 0;
}

int proc_fork(struct proc *parent, struct proc **out)
{
    struct proc *p;
    seL4_UserContext regs;

    int err = proc_alloc(parent->pid, &p);
    if (err != 0) {
        return err;
    }
    if ((err = build(p)) == 0 && (err = vms_clone(&p->vs, &parent->vs)) == 0 &&
        (err = kobj_sel4_err(seL4_TCB_ReadRegisters(parent->tcb.cap, 0, 0, ARCH_REGS_COUNT,
                                                    &regs))) == 0) {
        arch_regs_return_from_call(&regs, 0); /* the child sees fork() return 0 */
        err = start(p, &regs);
    }
    if (err != 0) {
        discard(p);
        return err;
    }
    *out = p;
    return 0;
}

/* A zombie child of `p` matching `pid` (-1 = any). *has_child says if any child matches at all. */
static struct proc *find_child(const struct proc *p, int32_t pid, bool *has_child)
{
    *has_child = false;
    for (unsigned i = 0; i < FASTIX_MAX_PROCS; i++) {
        struct proc *c = table[i];
        if (c == NULL || c->ppid != p->pid || (pid > 0 && c->pid != (uint32_t)pid)) {
            continue;
        }
        *has_child = true;
        if (c->zombie) {
            return c;
        }
    }
    return NULL;
}

int proc_wait(struct proc *p, int32_t pid, uint64_t options, uint32_t *child_pid, int *status)
{
    bool has_child;

    if (pid == 0 || pid < -1) {
        return -FASTIX_EINVAL; /* process-group waits arrive with process groups (Phase 3) */
    }
    struct proc *c = find_child(p, pid, &has_child);
    if (c != NULL) {
        *child_pid = c->pid;
        *status = c->status;
        proc_free(c); /* reaped */
        return 0;
    }
    if (!has_child) {
        return -FASTIX_ECHILD;
    }
    if (options & FASTIX_WNOHANG) {
        *child_pid = 0;
        *status = 0;
        return 0;
    }
    return 1;
}

/* If `parent` is parked in wait4 and a matching child has exited, complete that wait4. */
static void wake_waiter(struct proc *parent)
{
    uint32_t child_pid;
    int status;

    if (parent == NULL || parent->zombie || parent->wait_reply == NULL ||
        proc_wait(parent, parent->wait_pid, FASTIX_WNOHANG, &child_pid, &status) != 0 ||
        child_pid == 0) {
        return;
    }
    uint64_t mr[2] = { child_pid, (uint64_t)(int64_t)status };
    reply_send(parent->wait_reply, 2, mr);
    parent->wait_reply = NULL;
}

void proc_exit(struct proc *p, int status)
{
    libos_assert(!p->zombie);
    release(p);
    p->zombie = true;
    p->status = status;

    /* Orphans go to PID 1, which may be waiting for them. */
    bool gave_children = false;
    for (unsigned i = 0; i < FASTIX_MAX_PROCS && p->pid != 1; i++) {
        struct proc *c = table[i];
        if (c != NULL && c->ppid == p->pid) {
            c->ppid = 1;
            gave_children = true;
        }
    }
    if (gave_children) {
        wake_waiter(proc_get(1));
    }
    wake_waiter(proc_get(p->ppid));
}
