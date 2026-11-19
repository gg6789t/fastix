/* proc.c - process/thread table, spawn, fork, vfork, exit, wait, pgid/sid, creds (see proc.h). */
#include <arch/counter.h>
#include <arch/regs.h>
#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/cspace.h>
#include <fastix/errno.h>
#include <fastix/futex_abi.h>
#include <fastix/mman.h>
#include <fastix/signal_abi.h>
#include <fastix/vdso.h>
#include <fastix/vspace.h>
#include <libos/assert.h>
#include <libos/slab.h>

#include "cspace.h"
#include "futex.h"
#include "heap.h"
#include "pipe.h"
#include "proc.h"
#include "proc_internal.h"
#include "signal.h"
#include "timer.h"
#include "vdso.h"

/* Endpoint rights for clients: send, and receive a reply. */
#define DEFAULT_UMASK    022

static struct slab_cache proc_cache;
static struct slab_cache thread_cache;
static struct proc *table[FASTIX_MAX_PROCS]; /* slot = pid % FASTIX_MAX_PROCS */
static uint64_t n_procs;
static uint32_t last_pid;
static uint16_t next_gen = 1;
static seL4_CPtr endpoint;
static seL4_CPtr sched_control;

/* Root CNode guard so both L1 slots (0..61) and L2 CNodes (62..63) resolve in 64-bit CPtrs. */
static seL4_Word root_cspace_guard(void)
{
    seL4_Word guard_bits = seL4_WordBits - FASTIX_ROOT_CNODE_BITS - FASTIX_CSPACE_L2_BITS;
    return seL4_CNode_CapData_new(0, guard_bits).words[0];
}

void proc_module_init(seL4_CPtr ep, seL4_CPtr sc_control)
{
    slab_init(&proc_cache, "proc", sizeof(struct proc), heap_provider());
    slab_init(&thread_cache, "thread", sizeof(struct thread), heap_provider());
    endpoint = ep;
    sched_control = sc_control;
}

uint64_t proc_count(void)
{
    return n_procs;
}

struct proc *proc_table_at(unsigned idx)
{
    return idx < FASTIX_MAX_PROCS ? table[idx] : NULL;
}

seL4_CPtr proc_endpoint(void)
{
    return endpoint;
}

struct thread *proc_thread_alloc(void)
{
    return slab_alloc(&thread_cache);
}

struct thread *proc_find_thread(struct proc *p, uint32_t tid)
{
    struct list_node *n;
    LIST_FOREACH(n, &p->threads) {
        struct thread *t = LIST_ENTRY(n, struct thread, link);
        if (!t->exited && t->tid == tid) {
            return t;
        }
    }
    return NULL;
}

struct thread *proc_find_thread_global(uint32_t tid)
{
    for (unsigned i = 0; i < FASTIX_MAX_PROCS; i++) {
        struct proc *p = table[i];
        if (p == NULL || p->zombie) {
            continue;
        }
        struct thread *t = proc_find_thread(p, tid);
        if (t != NULL) {
            return t;
        }
    }
    return NULL;
}

struct proc *proc_get(uint32_t pid)
{
    if (pid == 0) {
        return NULL;
    }
    struct proc *p = table[pid % FASTIX_MAX_PROCS];
    return (p != NULL && p->pid == pid) ? p : NULL;
}

struct thread *proc_thread_from_badge(uint64_t badge)
{
    struct proc *p = proc_get(fastix_badge_pid(badge));
    if (p == NULL || p->zombie || p->gen != fastix_badge_gen(badge)) {
        return NULL;
    }
    uint32_t slot = fastix_badge_tid_slot(badge);
    struct list_node *n;
    LIST_FOREACH(n, &p->threads) {
        struct thread *t = LIST_ENTRY(n, struct thread, link);
        if (!t->exited && t->tid_slot == slot) {
            return t;
        }
    }
    return NULL;
}

/* Updates the read-only procinfo page using the seqlock protocol. */
void proc_sync_info(struct proc *p)
{
    if (p->procinfo == NULL) {
        return;
    }
    struct fastix_procinfo cur = { 0 };
    (void)frame_read(p->procinfo, 0, &cur, sizeof(cur));
    uint32_t seq = atomic_load_explicit(&cur.seq, memory_order_relaxed);
    seq = (seq & ~1u) + 2u; /* even version number; single-writer procd updates atomically */
    struct fastix_procinfo next = {
        .seq = seq,
        .pid = p->pid,
        .ppid = p->ppid,
        .pgid = p->pgid,
        .sid = p->sid,
        .uid = p->uid,
        .euid = p->euid,
        .gid = p->gid,
        .egid = p->egid,
        .umask = p->umask,
    };
    (void)frame_write(p->procinfo, 0, &next, sizeof(next), false);
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

static int proc_alloc(struct proc *parent, uint32_t ppid, struct proc **out)
{
    uint32_t pid;
    int err = pid_alloc(&pid);
    if (err != 0) {
        return err;
    }
    struct proc *p = slab_alloc(&proc_cache); /* zeroed */
    if (p == NULL) {
        return -FASTIX_ENOMEM;
    }
    p->pid = pid;
    p->gen = next_gen++;
    if (p->gen == 0) {
        p->gen = next_gen++;
    }
    p->ppid = ppid;
    if (parent != NULL) {
        p->pgid = parent->pgid;
        p->sid = parent->sid;
        p->uid = parent->uid;
        p->euid = parent->euid;
        p->suid = parent->suid;
        p->gid = parent->gid;
        p->egid = parent->egid;
        p->sgid = parent->sgid;
        p->umask = parent->umask;
    } else {
        p->pgid = pid;
        p->sid = pid;
        p->umask = DEFAULT_UMASK;
    }
    list_init(&p->threads);
    sig_proc_init(p, parent);
    timer_proc_init(p);
    pipe_proc_init(p, parent);
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

static void thread_release(struct thread *t)
{
    if (t->tcb.block != NULL) {
        (void)seL4_TCB_Suspend(t->tcb.cap);
        kobj_free(&t->tcb);
    }
    if (t->park_kind == PARK_SLEEP) {
        timer_cancel_sleep(t);
    } else if (t->park_kind == PARK_FUTEX) {
        futex_cancel_wait(t);
    } else if (t->park_kind == PARK_PIPE_READ || t->park_kind == PARK_PIPE_WRITE ||
               t->park_kind == PARK_EFD_READ || t->park_kind == PARK_EFD_WRITE) {
        pipe_cancel_wait(t);
    }
    if (t->park_reply != NULL) {
        reply_put(t->park_reply);
        t->park_reply = NULL;
        t->park_kind = PARK_NONE;
    }
    sig_thread_cleanup(t);
    if (t->sc.block != NULL) {
        kobj_free(&t->sc);
    }
    if (t->fault_ep != seL4_CapNull) {
        if (seL4_CNode_Delete(cspace_root(), t->fault_ep, seL4_WordBits) != seL4_NoError) {
            libos_assert(0 && "cannot delete fault endpoint copy");
        }
        kobj_slot_put(t->fault_ep);
        t->fault_ep = seL4_CapNull;
    }
    if (t->ipcbuf != NULL) {
        frame_put(t->ipcbuf);
        t->ipcbuf = NULL;
    }
}

/* Frees every resource of `p` (safe on a partly built process). */
static void release(struct proc *p)
{
    timer_proc_cleanup(p);
    sig_proc_cleanup(p);
    struct list_node *n;
    while ((n = list_pop(&p->threads)) != NULL) {
        struct thread *t = LIST_ENTRY(n, struct thread, link);
        thread_release(t);
        slab_free(&thread_cache, t);
    }
    p->main_thread = NULL;
    p->live_threads = 0;

    pipe_proc_cleanup(p);

    if (p->vs != NULL) {
        vms_put(p->vs);
        p->vs = NULL;
    }
    if (p->thread_cnode.block != NULL) {
        kobj_free(&p->thread_cnode);
    }
    if (p->cnode.block != NULL) {
        kobj_free(&p->cnode);
    }
    if (p->procinfo != NULL) {
        frame_put(p->procinfo);
        p->procinfo = NULL;
    }
}

/* Configures a thread's TCB, scheduling context, and badged fault endpoint. */
int proc_build_thread_objs(struct proc *p, struct thread *t, struct vmspace *vs,
                           uint64_t ipcbuf_vaddr)
{
    int err;
    if ((err = kobj_alloc(seL4_TCBObject, 0, &t->tcb)) != 0 ||
        (err = kobj_alloc(seL4_SchedContextObject, seL4_MinSchedContextBits, &t->sc)) != 0) {
        return err;
    }
    err = kobj_sel4_err(seL4_TCB_Configure(t->tcb.cap, p->cnode.cap, root_cspace_guard(),
                                           vs->root.cap, 0, ipcbuf_vaddr, t->ipcbuf->cap));
    if (err != 0) {
        return err;
    }
    err = kobj_sel4_err(seL4_SchedControl_ConfigureFlags(
        sched_control, t->sc.cap, FASTIX_DEFAULT_TIMESLICE_US, FASTIX_DEFAULT_TIMESLICE_US, 0, 0,
        0));
    if (err != 0) {
        return err;
    }
    if ((err = kobj_slot_get(&t->fault_ep)) != 0) {
        t->fault_ep = seL4_CapNull;
        return err;
    }
    uint64_t badge = fastix_badge_make(FASTIX_BADGE_FAULT, p->pid, t->tid_slot, p->gen);
    err = kobj_sel4_err(seL4_CNode_Mint(cspace_root(), t->fault_ep, seL4_WordBits, cspace_root(),
                                        endpoint, seL4_WordBits, CLIENT_EP_RIGHTS, badge));
    if (err != 0) {
        kobj_slot_put(t->fault_ep);
        t->fault_ep = seL4_CapNull;
        return err;
    }
    return kobj_sel4_err(seL4_TCB_SetSchedParams(t->tcb.cap, seL4_CapInitThreadTCB, 0,
                                                 FASTIX_DEFAULT_PRIO, t->sc.cap, t->fault_ep));
}

int proc_map_special_pages(struct proc *p, struct vmspace *vs, struct frame *ipcbuf)
{
    int err;
    if ((err = vms_add_special(vs, FASTIX_VDSO_ADDR, vdso_frame(), FASTIX_PROT_READ)) != 0 ||
        (err = vms_add_special(vs, FASTIX_VDSO_CODE_ADDR, vdso_code_frame(),
                               FASTIX_PROT_READ | FASTIX_PROT_EXEC)) != 0 ||
        (err = vms_add_special(vs, FASTIX_PROCINFO_ADDR, p->procinfo, FASTIX_PROT_READ)) != 0) {
        return err;
    }
    return vms_add_special(vs, FASTIX_IPCBUF_BASE, ipcbuf, FASTIX_PROT_READ | FASTIX_PROT_WRITE);
}

int proc_tcb_set_space(struct proc *p, struct thread *t, struct vmspace *vs)
{
    return kobj_sel4_err(seL4_TCB_SetSpace(t->tcb.cap, t->fault_ep, p->cnode.cap,
                                           root_cspace_guard(), vs->root.cap, 0));
}

int proc_thread_start(struct thread *t, seL4_UserContext *regs)
{
    regs->tpidrro_el0 = t->tid_slot;
    return kobj_sel4_err(seL4_TCB_WriteRegisters(t->tcb.cap, 1, 0, ARCH_REGS_COUNT, regs));
}

/* Allocates root CNode + badged main endpoint + procinfo + main thread shell. */
static int build_base_caps(struct proc *p, uint16_t main_slot)
{
    int err = kobj_alloc(seL4_CapTableObject, FASTIX_ROOT_CNODE_BITS, &p->cnode);
    if (err != 0) {
        return err;
    }
    uint64_t badge = fastix_badge_make(FASTIX_BADGE_SYSCALL, p->pid, main_slot, p->gen);
    err = kobj_sel4_err(seL4_CNode_Mint(p->cnode.cap, FASTIX_CAP_PROCD_EP_MAIN,
                                        FASTIX_ROOT_CNODE_BITS, cspace_root(), endpoint,
                                        seL4_WordBits, CLIENT_EP_RIGHTS, badge));
    if (err != 0) {
        return err;
    }
    err = frame_new(&p->procinfo);
    if (err != 0) {
        return err;
    }
    proc_sync_info(p);

    struct thread *t = slab_alloc(&thread_cache);
    if (t == NULL) {
        return -FASTIX_ENOMEM;
    }
    t->proc = p;
    t->tid = p->pid;
    t->tid_slot = main_slot;
    p->tid_slots[0] |= 1ULL;
    sig_thread_init(t, NULL);
    list_push(&p->threads, &t->link);
    p->main_thread = t;
    p->live_threads = 1;
    return 0;
}

int proc_create_empty(struct proc *parent, uint32_t ppid, struct proc **out)
{
    struct proc *p;
    int err = proc_alloc(parent, ppid, &p);
    if (err != 0) {
        return err;
    }
    if ((err = vms_create(&p->vs)) == 0 && (err = build_base_caps(p, 0)) == 0 &&
        (err = frame_new(&p->main_thread->ipcbuf)) == 0 &&
        (err = proc_map_special_pages(p, p->vs, p->main_thread->ipcbuf)) == 0 &&
        (err = proc_build_thread_objs(p, p->main_thread, p->vs, FASTIX_IPCBUF_BASE)) == 0) {
        *out = p;
        return 0;
    }
    proc_discard(p);
    return err;
}

void proc_discard(struct proc *p)
{
    release(p);
    proc_free(p);
}

int proc_spawn(const struct load_args *la, uint32_t ppid, struct proc **out)
{
    struct proc *p;
    uint64_t entry, sp;
    seL4_UserContext regs;

    int err = proc_create_empty(proc_get(ppid), ppid, &p);
    if (err != 0) {
        return err;
    }
    struct load_args args = *la;
    args.uid = p->uid;
    args.euid = p->euid;
    args.gid = p->gid;
    args.egid = p->egid;
    args.vdso_ehdr = FASTIX_VDSO_CODE_ADDR;
    if ((err = loader_load(p->vs, &args, &entry, &sp)) == 0) {
        arch_regs_init_entry(&regs, entry, sp, 0);
        err = proc_thread_start(p->main_thread, &regs);
    }
    if (err != 0) {
        proc_discard(p);
        return err;
    }
    *out = p;
    return 0;
}

int proc_fork(struct thread *caller, struct proc **out)
{
    struct proc *parent = caller->proc;
    struct proc *p;
    seL4_UserContext regs;

    int err = proc_create_empty(parent, parent->pid, &p);
    if (err != 0) {
        return err;
    }
    sig_thread_init(p->main_thread, caller);
    if ((err = vms_clone(p->vs, parent->vs)) == 0 &&
        (err = kobj_sel4_err(
             seL4_TCB_ReadRegisters(caller->tcb.cap, 0, 0, ARCH_REGS_COUNT, &regs))) == 0) {
        arch_regs_return_from_call(&regs, 0); /* the child sees fork() return 0 */
        err = proc_thread_start(p->main_thread, &regs);
    }
    if (err != 0) {
        proc_discard(p);
        return err;
    }
    *out = p;
    return 0;
}

int proc_vfork(struct thread *caller, struct reply_obj *parent_reply, struct proc **out)
{
    struct proc *parent = caller->proc;
    struct proc *p;
    seL4_UserContext regs;

    int err = proc_alloc(parent, parent->pid, &p);
    if (err != 0) {
        return err;
    }
    vms_get(parent->vs);
    p->vs = parent->vs;
    p->vfork_parent_pid = parent->pid;

    /* Child borrows caller's IPC buffer (caller stays parked in vfork until child exits/execs). */
    err = build_base_caps(p, 0);
    if (err == 0) {
        sig_thread_init(p->main_thread, caller);
        frame_get(caller->ipcbuf);
        p->main_thread->ipcbuf = caller->ipcbuf;
        uint64_t ipcbuf_vaddr = FASTIX_IPCBUF_BASE +
                                (uint64_t)caller->tid_slot * FASTIX_IPCBUF_STRIDE;
        err = proc_build_thread_objs(p, p->main_thread, p->vs, ipcbuf_vaddr);
    }
    if (err == 0) {
        err = kobj_sel4_err(seL4_TCB_ReadRegisters(caller->tcb.cap, 0, 0, ARCH_REGS_COUNT, &regs));
    }
    if (err == 0) {
        arch_regs_return_from_call(&regs, 0); /* child sees vfork() return 0 */
        /* Note: child's tpidrro_el0 = 0 uses CAP_PROCD_EP_MAIN; caller's slot 0 IPC buffer. */
        err = proc_thread_start(p->main_thread, &regs);
    }
    if (err != 0) {
        proc_discard(p);
        return err;
    }
    caller->park_reply = parent_reply;
    caller->park_kind = PARK_VFORK;
    *out = p;
    return 0;
}

void proc_vfork_wake_parent(struct proc *child)
{
    if (child->vfork_parent_pid == 0) {
        return;
    }
    struct proc *parent = proc_get(child->vfork_parent_pid);
    child->vfork_parent_pid = 0;
    if (parent == NULL || parent->zombie) {
        return;
    }
    struct list_node *n;
    LIST_FOREACH(n, &parent->threads) {
        struct thread *t = LIST_ENTRY(n, struct thread, link);
        if (t->park_kind == PARK_VFORK && t->park_reply != NULL) {
            uint64_t mr[1] = { child->pid };
            struct reply_obj *r = t->park_reply;
            t->park_reply = NULL;
            t->park_kind = PARK_NONE;
            reply_send(r, 1, mr);
            return;
        }
    }
}

void proc_thread_free(struct thread *t)
{
    thread_release(t);
    slab_free(&thread_cache, t);
}

/* ---- wait4 & exit ---- */

static bool child_matches_wait(const struct proc *parent, const struct proc *c, int32_t pid)
{
    if (c == NULL || c->ppid != parent->pid) {
        return false;
    }
    if (pid == -1) {
        return true;
    }
    if (pid > 0) {
        return c->pid == (uint32_t)pid;
    }
    if (pid == 0) {
        return c->pgid == parent->pgid;
    }
    return c->pgid == (uint32_t)(-pid);
}

int proc_wait(struct proc *p, int32_t pid, uint64_t options, uint32_t *child_pid, int *status)
{
    uint64_t allowed = FASTIX_WNOHANG | FASTIX_WUNTRACED | FASTIX_WCONTINUED;
    if ((options & ~allowed) != 0) {
        return -FASTIX_EINVAL;
    }
    bool has_child = false;
    for (unsigned i = 0; i < FASTIX_MAX_PROCS; i++) {
        struct proc *c = table[i];
        if (!child_matches_wait(p, c, pid)) {
            continue;
        }
        has_child = true;
        if (c->zombie) {
            *child_pid = c->pid;
            *status = c->status;
            proc_free(c);
            return 0;
        }
        if ((options & FASTIX_WUNTRACED) && c->stopped && !c->stop_reported) {
            c->stop_reported = true;
            *child_pid = c->pid;
            *status = FASTIX_W_STOPPED(c->stop_sig);
            return 0;
        }
        if ((options & FASTIX_WCONTINUED) && c->cont_pending) {
            c->cont_pending = false;
            *child_pid = c->pid;
            *status = FASTIX_W_CONTINUED;
            return 0;
        }
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

void proc_wake_waiter(struct proc *parent)
{
    if (parent == NULL || parent->zombie) {
        return;
    }
    struct list_node *n;
    LIST_FOREACH(n, &parent->threads) {
        struct thread *t = LIST_ENTRY(n, struct thread, link);
        if (t->park_kind != PARK_WAIT4 || t->park_reply == NULL) {
            continue;
        }
        uint32_t child_pid = 0;
        int status = 0;
        int r = proc_wait(parent, t->wait_pid, t->wait_options | FASTIX_WNOHANG, &child_pid,
                          &status);
        if (r == 0 && child_pid == 0) {
            continue;
        }
        if (r == 0 && t->wait_status_ptr != 0) {
            int32_t st32 = (int32_t)status;
            int werr = vms_write_user(parent->vs, t->wait_status_ptr, &st32, sizeof(st32));
            if (werr != 0) {
                r = werr;
            }
        }
        uint64_t mr[2] = { r < 0 ? (uint64_t)(int64_t)r : child_pid, (uint64_t)(int64_t)status };
        struct reply_obj *rep = t->park_reply;
        t->park_reply = NULL;
        t->park_kind = PARK_NONE;
        reply_send(rep, 2, mr);
    }
}

void proc_exit(struct proc *p, int status)
{
    libos_assert(!p->zombie);
    uint32_t ppid = p->ppid;
    /* Run robust futex cleanup for any live threads before tearing down the address space. */
    struct list_node *tn;
    LIST_FOREACH(tn, &p->threads) {
        struct thread *t = LIST_ENTRY(tn, struct thread, link);
        if (!t->exited) {
            futex_exit_robust_list(t);
        }
    }
    release(p);
    p->zombie = true;
    p->stopped = false;
    p->status = status;

    /* If this was a vfork child that exited without execve, unblock parent now. */
    proc_vfork_wake_parent(p);

    /* Orphans go to PID 1, which may be waiting for them. */
    bool gave_zombie = false;
    for (unsigned i = 0; i < FASTIX_MAX_PROCS && p->pid != 1; i++) {
        struct proc *c = table[i];
        if (c != NULL && c->ppid == p->pid) {
            c->ppid = 1;
            if (!c->zombie) {
                proc_sync_info(c);
            } else {
                gave_zombie = true;
            }
        }
    }
    if (gave_zombie) {
        struct proc *init = proc_get(1);
        if (init != NULL && !init->zombie) {
            sig_send_child(init, p, FASTIX_CLD_EXITED, 0);
            proc_wake_waiter(init);
        }
    }

    struct proc *parent = proc_get(ppid);
    if (parent != NULL && !parent->zombie) {
        int sig = FASTIX_W_TERMSIG(status);
        int cld_code = sig != 0 ? FASTIX_CLD_KILLED : FASTIX_CLD_EXITED;
        int cld_status = sig != 0 ? sig : FASTIX_W_EXITCODE(status);
        sig_send_child(parent, p, cld_code, cld_status);
        if (sig_should_autoreap(parent)) {
            proc_free(p);
        }
        proc_wake_waiter(parent);
    }
}

