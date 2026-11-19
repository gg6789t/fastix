/* thread.c - thread creation and exit (split from proc.c). */
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

static uint32_t next_worker_tid = FASTIX_PID_MAX + 1; /* tids for extra threads, above all pids */

/* ---- threads (Step 3C) ---- */

static int tid_slot_alloc(struct proc *p, uint16_t *out)
{
    for (unsigned w = 0; w < FASTIX_MAX_THREADS_PER_PROC / 64; w++) {
        if (~p->tid_slots[w] == 0) {
            continue;
        }
        unsigned bit = (unsigned)__builtin_ctzll(~p->tid_slots[w]);
        unsigned slot = w * 64u + bit;
        if (slot == 0 || slot >= FASTIX_MAX_THREADS_PER_PROC) {
            continue;
        }
        p->tid_slots[w] |= 1ULL << bit;
        *out = (uint16_t)slot;
        return 0;
    }
    return -FASTIX_EAGAIN;
}

static void tid_slot_free(struct proc *p, uint16_t slot)
{
    if (slot == 0 || slot >= FASTIX_MAX_THREADS_PER_PROC) {
        return;
    }
    p->tid_slots[slot / 64u] &= ~(1ULL << (slot % 64u));
}

static uint32_t worker_tid_alloc(void)
{
    uint32_t tid = next_worker_tid++;
    if (next_worker_tid > FASTIX_BADGE_PID_MASK) {
        next_worker_tid = FASTIX_PID_MAX + 1;
    }
    return tid;
}

/* Ensures the level-2 thread CNode is allocated and installed at root slot 63. */
static int ensure_thread_cnode(struct proc *p)
{
    if (p->thread_cnode.block != NULL) {
        return 0;
    }
    int err = kobj_alloc(seL4_CapTableObject, FASTIX_THREAD_CNODE_BITS, &p->thread_cnode);
    if (err != 0) {
        return err;
    }
    err = kobj_sel4_err(seL4_CNode_Copy(p->cnode.cap, FASTIX_CAP_THREAD_CNODE,
                                        FASTIX_ROOT_CNODE_BITS, cspace_root(), p->thread_cnode.cap,
                                        seL4_WordBits, seL4_AllRights));
    if (err != 0) {
        kobj_free(&p->thread_cnode);
    }
    return err;
}

int proc_thread_create(struct thread *caller, uint64_t entry, uint64_t stack, uint64_t arg,
                       uint64_t tls, uint64_t ctid_ptr, uint32_t flags, uint32_t *tid_out)
{
    struct proc *p = caller->proc;
    if (entry == 0 || stack == 0) {
        return -FASTIX_EINVAL;
    }
    int err = ensure_thread_cnode(p);
    if (err != 0) {
        return err;
    }
    uint16_t slot;
    err = tid_slot_alloc(p, &slot);
    if (err != 0) {
        return err;
    }
    struct thread *t = proc_thread_alloc();
    if (t == NULL) {
        tid_slot_free(p, slot);
        return -FASTIX_ENOMEM;
    }
    t->proc = p;
    t->tid = worker_tid_alloc();
    t->tid_slot = slot;
    sig_thread_init(t, caller);

    uint64_t ipcbuf_vaddr = FASTIX_IPCBUF_BASE + (uint64_t)slot * FASTIX_IPCBUF_STRIDE;
    uint64_t badge = fastix_badge_make(FASTIX_BADGE_SYSCALL, p->pid, slot, p->gen);
    bool ep_minted = false, ipcbuf_mapped = false;

    err = kobj_sel4_err(seL4_CNode_Mint(p->thread_cnode.cap, (seL4_Word)slot * 2u,
                                        FASTIX_THREAD_CNODE_BITS, cspace_root(), proc_endpoint(),
                                        seL4_WordBits, CLIENT_EP_RIGHTS, badge));
    if (err == 0) {
        ep_minted = true;
        err = frame_new(&t->ipcbuf);
    }
    if (err == 0) {
        err = vms_add_special(p->vs, ipcbuf_vaddr, t->ipcbuf, FASTIX_PROT_READ | FASTIX_PROT_WRITE);
    }
    if (err == 0) {
        ipcbuf_mapped = true;
        err = proc_build_thread_objs(p, t, p->vs, ipcbuf_vaddr);
    }
    if (err == 0 && (flags & FASTIX_CLONE_CHILD_SETTID) && ctid_ptr != 0) {
        uint32_t tid32 = t->tid;
        err = vms_write_user(p->vs, ctid_ptr, &tid32, sizeof(tid32));
    }
    if (err == 0) {
        if (flags & FASTIX_CLONE_CHILD_CLEARTID) {
            t->clear_child_tid = ctid_ptr;
        }
        seL4_UserContext regs;
        arch_regs_init_entry(&regs, entry, stack & ~15ULL, arg);
        if (flags & FASTIX_CLONE_SETTLS) {
            regs.tpidr_el0 = tls;
        }
        list_push_back(&p->threads, &t->link);
        p->live_threads++;
        err = proc_thread_start(t, &regs);
        if (err != 0) {
            list_remove(&t->link);
            p->live_threads--;
        }
    }
    if (err != 0) {
        if (ipcbuf_mapped) {
            vms_remove_special(p->vs, ipcbuf_vaddr);
        }
        if (ep_minted) {
            (void)seL4_CNode_Delete(p->thread_cnode.cap, (seL4_Word)slot * 2u,
                                    FASTIX_THREAD_CNODE_BITS);
        }
        proc_thread_free(t);
        tid_slot_free(p, slot);
        return err;
    }
    *tid_out = t->tid;
    return 0;
}

static void destroy_single_thread(struct proc *p, struct thread *t)
{
    uint16_t slot = t->tid_slot;
    uint64_t ipcbuf_vaddr = FASTIX_IPCBUF_BASE + (uint64_t)slot * FASTIX_IPCBUF_STRIDE;

    list_remove(&t->link);
    p->live_threads--;
    if (p->main_thread == t) {
        p->main_thread = list_empty(&p->threads)
                             ? NULL
                             : LIST_ENTRY(p->threads.next, struct thread, link);
    }
    vms_remove_special(p->vs, ipcbuf_vaddr);
    if (slot != 0 && p->thread_cnode.block != NULL) {
        (void)seL4_CNode_Delete(p->thread_cnode.cap, (seL4_Word)slot * 2u,
                                FASTIX_THREAD_CNODE_BITS);
    }
    proc_thread_free(t);
    tid_slot_free(p, slot);
}

void proc_thread_exit(struct thread *t, int code)
{
    struct proc *p = t->proc;
    futex_exit_robust_list(t);

    if (t->clear_child_tid != 0 && p->vs != NULL) {
        uint32_t zero = 0;
        if (vms_write_user(p->vs, t->clear_child_tid, &zero, sizeof(zero)) == 0) {
            (void)futex_wake_internal(p->vs, t->clear_child_tid, 1, true,
                                      FASTIX_FUTEX_BITSET_MATCH_ANY);
            (void)futex_wake_internal(p->vs, t->clear_child_tid, 1, false,
                                      FASTIX_FUTEX_BITSET_MATCH_ANY);
        }
        t->clear_child_tid = 0;
    }

    if (p->live_threads <= 1) {
        proc_exit(p, FASTIX_W_EXITED(code));
        return;
    }
    destroy_single_thread(p, t);
}

void proc_kill_other_threads(struct thread *caller)
{
    struct proc *p = caller->proc;
    struct list_node *n, *tmp;
    LIST_FOREACH_SAFE(n, tmp, &p->threads) {
        struct thread *t = LIST_ENTRY(n, struct thread, link);
        if (t != caller) {
            futex_exit_robust_list(t);
            destroy_single_thread(p, t);
        }
    }
    if (caller->tid_slot != 0) {
        /* Move surviving execve caller to canonical slot 0 (main thread). */
        uint16_t old_slot = caller->tid_slot;
        caller->tid_slot = 0;
        caller->tid = p->pid;
        p->main_thread = caller;
        tid_slot_free(p, old_slot);
    }
    if (p->thread_cnode.block != NULL) {
        (void)seL4_CNode_Delete(p->cnode.cap, FASTIX_CAP_THREAD_CNODE, FASTIX_ROOT_CNODE_BITS);
        kobj_free(&p->thread_cnode);
    }
}
