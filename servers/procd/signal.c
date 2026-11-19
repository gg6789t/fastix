/* signal.c - POSIX signal queuing, delivery, stop/continue, and syscalls (see signal.h). */
#include <stddef.h>
#include <stdint.h>

#include <arch/regs.h>
#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/cspace.h>
#include <fastix/errno.h>
#include <fastix/signal_abi.h>
#include <fastix/vspace.h>
#include <libos/assert.h>
#include <libos/mem.h>
#include <libos/slab.h>

#include "futex.h"
#include "heap.h"
#include "pipe.h"
#include "sig_internal.h"
#include "signal.h"
#include "timer.h"

_Static_assert(((FASTIX_SYS_rt_sigreturn << 12) | 1) == FASTIX_SIGRETURN_MSGINFO,
               "FASTIX_SIGRETURN_MSGINFO out of sync with FASTIX_SYS_rt_sigreturn");

#define STOP_MASK        (fastix_sig_bit(FASTIX_SIGSTOP) | fastix_sig_bit(FASTIX_SIGTSTP) | \
                          fastix_sig_bit(FASTIX_SIGTTIN) | fastix_sig_bit(FASTIX_SIGTTOU))


struct sigq_item {
    struct list_node link;
    struct fastix_siginfo info;
};

static struct slab_cache sigq_cache;
static struct thread *active_caller;

void sig_module_init(void)
{
    slab_init(&sigq_cache, "sigq", sizeof(struct sigq_item), heap_provider());
}

void sig_set_active_caller(struct thread *t)
{
    active_caller = t;
}

bool sig_valid(int sig)
{
    return sig >= 1 && sig < FASTIX_NSIG;
}

enum def_action sig_default_action(int sig)
{
    switch (sig) {
    case FASTIX_SIGCHLD:
    case FASTIX_SIGURG:
    case FASTIX_SIGWINCH:
        return DEF_IGN;
    case FASTIX_SIGSTOP:
    case FASTIX_SIGTSTP:
    case FASTIX_SIGTTIN:
    case FASTIX_SIGTTOU:
        return DEF_STOP;
    case FASTIX_SIGCONT:
        return DEF_CONT;
    default:
        return DEF_TERM;
    }
}

/* ---- lifecycle ---- */

void sig_proc_init(struct proc *p, const struct proc *parent)
{
    list_init(&p->sigq);
    p->pending_mask = 0;
    p->sigq_count = 0;
    if (parent != NULL) {
        memcpy(p->sigact, parent->sigact, sizeof(p->sigact));
    } else {
        memset(p->sigact, 0, sizeof(p->sigact));
    }
}

void sig_thread_init(struct thread *t, const struct thread *parent_t)
{
    list_init(&t->sigq);
    t->pending_mask = 0;
    t->sigq_count = 0;
    t->has_saved_sigmask = false;
    if (parent_t != NULL) {
        t->sigmask = parent_t->sigmask;
        t->altstack = parent_t->altstack;
    } else {
        t->sigmask = 0;
        t->altstack = (struct fastix_sigaltstack){ .ss_flags = FASTIX_SS_DISABLE };
    }
}

static void free_sigq(struct list_node *q, uint32_t *count)
{
    struct list_node *n;
    while ((n = list_pop(q)) != NULL) {
        slab_free(&sigq_cache, LIST_ENTRY(n, struct sigq_item, link));
    }
    *count = 0;
}

void sig_proc_cleanup(struct proc *p)
{
    free_sigq(&p->sigq, &p->sigq_count);
    p->pending_mask = 0;
}

void sig_thread_cleanup(struct thread *t)
{
    free_sigq(&t->sigq, &t->sigq_count);
    t->pending_mask = 0;
}

void sig_on_exec(struct proc *p, struct thread *t)
{
    /* POSIX: caught handlers reset to SIG_DFL; SIG_IGN stays SIG_IGN; mask/pending preserved. */
    for (int sig = 1; sig < FASTIX_NSIG; sig++) {
        if (p->sigact[sig].handler != FASTIX_SIG_DFL &&
            p->sigact[sig].handler != FASTIX_SIG_IGN) {
            p->sigact[sig] = (struct fastix_sigaction){ 0 };
        }
    }
    t->altstack = (struct fastix_sigaltstack){ .ss_flags = FASTIX_SS_DISABLE };
}

bool sig_should_autoreap(const struct proc *parent)
{
    if (parent == NULL || parent->zombie) {
        return false;
    }
    const struct fastix_sigaction *sa = &parent->sigact[FASTIX_SIGCHLD];
    return sa->handler == FASTIX_SIG_IGN || (sa->flags & FASTIX_SA_NOCLDWAIT) != 0;
}

/* ---- queue helpers ---- */

static int sigq_enqueue(struct list_node *q, uint64_t *mask, uint32_t *count, int sig,
                        const struct fastix_siginfo *info)
{
    uint64_t bit = fastix_sig_bit(sig);
    if (sig < FASTIX_SIGRTMIN && (*mask & bit) != 0) {
        return 0; /* standard signals coalesce */
    }
    if (*count >= FASTIX_SIGQUEUE_MAX) {
        return sig >= FASTIX_SIGRTMIN ? -FASTIX_EAGAIN : 0;
    }
    struct sigq_item *item = slab_alloc(&sigq_cache);
    if (item == NULL) {
        return -FASTIX_ENOMEM;
    }
    if (info != NULL) {
        item->info = *info;
    }
    item->info.si_signo = sig;
    list_push_back(q, &item->link);
    (*count)++;
    *mask |= bit;
    return 0;
}

/* Removes one entry for `sig` from queue `q` (FIFO), clearing *mask bit if none left. */
static bool sigq_pop_sig(struct list_node *q, uint64_t *mask, uint32_t *count, int sig,
                         struct fastix_siginfo *out)
{
    struct list_node *n, *tmp;
    bool found = false, more = false;
    LIST_FOREACH_SAFE(n, tmp, q) {
        struct sigq_item *it = LIST_ENTRY(n, struct sigq_item, link);
        if (it->info.si_signo != sig) {
            continue;
        }
        if (!found) {
            *out = it->info;
            list_remove(n);
            slab_free(&sigq_cache, it);
            (*count)--;
            found = true;
        } else {
            more = true;
            break;
        }
    }
    if (!more) {
        *mask &= ~fastix_sig_bit(sig);
    }
    return found;
}

void sig_discard_mask(struct list_node *q, uint64_t *mask, uint32_t *count,
                              uint64_t drop_bits)
{
    struct list_node *n, *tmp;
    LIST_FOREACH_SAFE(n, tmp, q) {
        struct sigq_item *it = LIST_ENTRY(n, struct sigq_item, link);
        if ((fastix_sig_bit(it->info.si_signo) & drop_bits) != 0) {
            list_remove(n);
            slab_free(&sigq_cache, it);
            (*count)--;
        }
    }
    *mask &= ~drop_bits;
}

/* Returns lowest unblocked pending signal for `t` (1..64), or 0 if none. */
int sig_next_deliverable(const struct thread *t)
{
    uint64_t avail = (t->pending_mask | t->proc->pending_mask) & ~t->sigmask;
    if (avail == 0) {
        return 0;
    }
    return __builtin_ctzll(avail) + 1;
}

static bool dequeue_signal(struct thread *t, int sig, struct fastix_siginfo *info)
{
    if (sigq_pop_sig(&t->sigq, &t->pending_mask, &t->sigq_count, sig, info)) {
        return true;
    }
    return sigq_pop_sig(&t->proc->sigq, &t->proc->pending_mask, &t->proc->sigq_count, sig, info);
}

/* ---- stop & continue ---- */

static void stop_process(struct proc *p, int sig)
{
    if (p->zombie || p->pid == 1 || p->stopped) {
        return;
    }
    p->stopped = true;
    p->stop_sig = sig;
    p->stop_reported = false;
    p->cont_pending = false;

    struct list_node *n;
    LIST_FOREACH(n, &p->threads) {
        struct thread *t = LIST_ENTRY(n, struct thread, link);
        if (!t->exited && t->park_kind == PARK_NONE) {
            (void)seL4_TCB_Suspend(t->tcb.cap);
        }
    }
    struct proc *parent = proc_get(p->ppid);
    sig_send_child(parent, p, FASTIX_CLD_STOPPED, sig);
    proc_wake_waiter(parent);
}

static void continue_process(struct proc *p)
{
    if (p->zombie) {
        return;
    }
    /* SIGCONT always clears any pending stop signals. */
    sig_discard_mask(&p->sigq, &p->pending_mask, &p->sigq_count, STOP_MASK);
    struct list_node *n;
    LIST_FOREACH(n, &p->threads) {
        struct thread *t = LIST_ENTRY(n, struct thread, link);
        sig_discard_mask(&t->sigq, &t->pending_mask, &t->sigq_count, STOP_MASK);
    }
    if (!p->stopped) {
        return;
    }
    p->stopped = false;
    p->cont_pending = true;
    LIST_FOREACH(n, &p->threads) {
        struct thread *t = LIST_ENTRY(n, struct thread, link);
        if (!t->exited && t->park_kind == PARK_NONE) {
            (void)seL4_TCB_Resume(t->tcb.cap);
        }
    }
    struct proc *parent = proc_get(p->ppid);
    sig_send_child(parent, p, FASTIX_CLD_CONTINUED, FASTIX_SIGCONT);
    proc_wake_waiter(parent);
}

/* ---- frame delivery ---- */

bool sig_on_altstack(const struct thread *t, uint64_t sp)
{
    if (t->altstack.ss_flags & FASTIX_SS_DISABLE) {
        return false;
    }
    return sp >= t->altstack.ss_sp && sp < t->altstack.ss_sp + t->altstack.ss_size;
}

/* Pushes a signal frame onto `t`'s user stack and resumes `t` into the VDSO entry trampoline. */
static bool push_sigframe_and_run(struct thread *t, int sig, const struct fastix_siginfo *info,
                                  const seL4_UserContext *saved_regs, uint64_t restore_mask)
{
    struct proc *p = t->proc;
    struct fastix_sigaction *sa = &p->sigact[sig];
    uint64_t handler = sa->handler;
    uint64_t flags = sa->flags;

    bool already_alt = sig_on_altstack(t, saved_regs->sp);
    bool use_alt = (flags & FASTIX_SA_ONSTACK) && !(t->altstack.ss_flags & FASTIX_SS_DISABLE) &&
                   !already_alt;
    uint64_t top_sp = use_alt ? (t->altstack.ss_sp + t->altstack.ss_size) : saved_regs->sp;
    if (top_sp < sizeof(struct fastix_sigframe) + 16) {
        return false;
    }
    uint64_t frame_sp = (top_sp - sizeof(struct fastix_sigframe)) & ~15ULL;

    struct fastix_sigframe frame = { 0 };
    frame.magic = FASTIX_SIGFRAME_MAGIC;
    frame.handler = handler;
    frame.procd_ep = fastix_cptr_thread_ep(t->tid_slot);
    frame.saved_on_altstack = already_alt ? 1 : 0;
    frame.info = *info;
    frame.uc.uc_stack = t->altstack;
    frame.uc.uc_sigmask = restore_mask;
    arch_regs_to_sigctx(saved_regs, &frame.uc.uc_mcontext);
    frame.uc.uc_mcontext.fault_addr = info->si_addr;

    if (vms_write_user(p->vs, frame_sp, &frame, sizeof(frame)) != 0) {
        return false; /* stack overflow: caller kills process with SIGSEGV */
    }

    if (flags & FASTIX_SA_RESETHAND) {
        sa->handler = FASTIX_SIG_DFL;
    }
    uint64_t new_mask = t->sigmask | sa->mask;
    if (!(flags & FASTIX_SA_NODEFER)) {
        new_mask |= fastix_sig_bit(sig);
    }
    t->sigmask = new_mask & ~UNCATCHABLE_MASK;

    seL4_UserContext next = *saved_regs;
    next.sp = frame_sp;
    next.pc = FASTIX_VDSO_CODE_ADDR + FASTIX_VDSO_SIGENTER_OFF;
    next.x0 = (uint64_t)sig;
    next.x1 = frame_sp + offsetof(struct fastix_sigframe, info);
    next.x2 = frame_sp + offsetof(struct fastix_sigframe, uc);
    next.x3 = handler;
    next.x30 = FASTIX_VDSO_CODE_ADDR + FASTIX_VDSO_SIGRETURN_OFF;
    return proc_thread_start(t, &next) == 0;
}

/*
 * Delivers one pending unblocked signal to `t` using `base_regs` (already suspended).
 * Returns true if `t` was handled (resumed into handler, stopped, or killed);
 * false if no deliverable user/stop/term signal remained (all pending were ignored).
 */
bool sig_deliver_pending(struct thread *t, const seL4_UserContext *base_regs)
{
    struct proc *p = t->proc;

    for (;;) {
        int sig = sig_next_deliverable(t);
        if (sig == 0) {
            return false;
        }
        struct fastix_siginfo info = { 0 };
        if (!dequeue_signal(t, sig, &info)) {
            continue;
        }
        uint64_t handler = p->sigact[sig].handler;
        if (handler == FASTIX_SIG_IGN) {
            continue;
        }
        if (handler == FASTIX_SIG_DFL) {
            enum def_action act = sig_default_action(sig);
            if (act == DEF_IGN || act == DEF_CONT) {
                continue;
            }
            if (act == DEF_STOP) {
                stop_process(p, sig);
                return true;
            }
            if (p->pid == 1 && (sig == FASTIX_SIGKILL || sig == FASTIX_SIGTERM)) {
                continue; /* PID 1 ignores unhandled SIGKILL/SIGTERM */
            }
            proc_exit(p, FASTIX_W_SIGNALED(sig));
            return true;
        }
        uint64_t restore_mask = t->has_saved_sigmask ? t->saved_sigmask : t->sigmask;
        t->has_saved_sigmask = false;
        if (!push_sigframe_and_run(t, sig, &info, base_regs, restore_mask)) {
            proc_exit(p, FASTIX_W_SIGNALED(FASTIX_SIGSEGV));
        }
        return true;
    }
}

/* True if the syscall `t` is parked in is restartable under SA_RESTART. */
static bool park_is_restartable(enum park_kind kind)
{
    return kind == PARK_WAIT4 || kind == PARK_PIPE_READ || kind == PARK_PIPE_WRITE ||
           kind == PARK_EFD_READ || kind == PARK_EFD_WRITE;
}

int sig_take_waited(struct thread *t, uint64_t set, uint64_t info_u)
{
    uint64_t ready = (t->pending_mask | t->proc->pending_mask) & set;
    if (ready == 0) {
        return 0;
    }
    int sig = 1 + __builtin_ctzll(ready); /* fastix_sig_bit(sig) == 1 << (sig - 1) */
    struct fastix_siginfo info;
    if (!dequeue_signal(t, sig, &info)) {
        return 0;
    }
    if (info_u != 0) {
        int err = vms_write_user(t->proc->vs, info_u, &info, sizeof(info));
        if (err != 0) {
            return err;
        }
    }
    return sig;
}

/* Hands `sig` to a thread parked in rt_sigtimedwait for it (`only`: just that thread). */
static bool wake_sigwaiter(struct proc *p, const struct thread *only, int sig)
{
    struct list_node *n;
    LIST_FOREACH(n, &p->threads) {
        struct thread *t = LIST_ENTRY(n, struct thread, link);
        if ((only != NULL && t != only) || t->exited || t->park_kind != PARK_SIGWAIT ||
            (t->sigwait_set & fastix_sig_bit(sig)) == 0) {
            continue;
        }
        int r = sig_take_waited(t, fastix_sig_bit(sig), t->sigwait_info_ptr);
        if (r == 0) {
            return false;
        }
        timer_cancel_sleep(t); /* no-op unless the wait had a timeout */
        struct reply_obj *rep = t->park_reply;
        t->park_reply = NULL;
        t->park_kind = PARK_NONE;
        uint64_t mr[1] = { (uint64_t)(int64_t)r };
        reply_send(rep, 1, mr);
        return true;
    }
    return false;
}

/* Unparks `t` due to an unblocked signal and delivers it (handling SA_RESTART vs EINTR). */
static void interrupt_parked_thread(struct thread *t)
{
    struct proc *p = t->proc;
    int sig = sig_next_deliverable(t);
    if (sig == 0) {
        return;
    }
    uint64_t handler = p->sigact[sig].handler;
    if (handler == FASTIX_SIG_IGN) {
        struct fastix_siginfo dummy;
        (void)dequeue_signal(t, sig, &dummy);
        return;
    }
    if (handler == FASTIX_SIG_DFL) {
        enum def_action act = sig_default_action(sig);
        if (act == DEF_IGN || act == DEF_CONT) {
            struct fastix_siginfo dummy;
            (void)dequeue_signal(t, sig, &dummy);
            return;
        }
        if (act == DEF_STOP) {
            struct fastix_siginfo dummy;
            (void)dequeue_signal(t, sig, &dummy);
            stop_process(p, sig);
            return;
        }
        if (p->pid == 1 && (sig == FASTIX_SIGKILL || sig == FASTIX_SIGTERM)) {
            struct fastix_siginfo dummy;
            (void)dequeue_signal(t, sig, &dummy);
            return;
        }
        proc_exit(p, FASTIX_W_SIGNALED(sig));
        return;
    }

    /* User handler: unpark the thread cleanly and push the signal frame. */
    enum park_kind kind = t->park_kind;
    uint64_t partial = 0;
    if (kind == PARK_SLEEP || kind == PARK_SIGWAIT) {
        timer_cancel_sleep(t);
    } else if (kind == PARK_FUTEX) {
        futex_cancel_wait(t);
    } else if (kind == PARK_PIPE_READ || kind == PARK_PIPE_WRITE || kind == PARK_EFD_READ ||
               kind == PARK_EFD_WRITE) {
        partial = (kind == PARK_PIPE_WRITE) ? t->io_done : 0;
        pipe_cancel_wait(t);
    }
    if (t->park_reply != NULL) {
        reply_put(t->park_reply);
        t->park_reply = NULL;
    }
    t->park_kind = PARK_NONE;

    seL4_UserContext regs;
    if (seL4_TCB_ReadRegisters(t->tcb.cap, 1, 0, ARCH_REGS_COUNT, &regs) != seL4_NoError) {
        proc_exit(p, FASTIX_W_SIGNALED(FASTIX_SIGKILL));
        return;
    }
    if (partial > 0) {
        arch_regs_return_from_call(&regs, partial);
    } else {
        bool restart = park_is_restartable(kind) && (p->sigact[sig].flags & FASTIX_SA_RESTART) != 0;
        if (!restart) {
            arch_regs_return_from_call(&regs, (uint64_t)-FASTIX_EINTR);
        }
    }
    if (!sig_deliver_pending(t, &regs)) {
        (void)proc_thread_start(t, &regs);
    }
}

/* Attempts immediate delivery to a target thread (if not the active syscall caller). */
static void kick_thread(struct thread *t)
{
    if (t == NULL || t->exited || t->proc->zombie || t->proc->stopped) {
        return;
    }
    if (sig_next_deliverable(t) == 0) {
        return;
    }
    if (t == active_caller) {
        return; /* sig_check_on_return() will deliver before replying */
    }
    if (t->park_kind == PARK_VFORK) {
        return; /* vfork parent stays blocked until child exits/execs */
    }
    if (t->park_kind != PARK_NONE) {
        interrupt_parked_thread(t);
        return;
    }
    seL4_UserContext regs;
    if (seL4_TCB_ReadRegisters(t->tcb.cap, 1, 0, ARCH_REGS_COUNT, &regs) != seL4_NoError) {
        return;
    }
    if (!sig_deliver_pending(t, &regs)) {
        (void)seL4_TCB_Resume(t->tcb.cap);
    }
}

/* Picks a live thread in `p` that has `sig` unblocked (preferring parked/sigsuspend). */
static struct thread *pick_target_thread(struct proc *p, int sig)
{
    uint64_t bit = fastix_sig_bit(sig);
    struct thread *best = NULL;
    struct list_node *n;

    LIST_FOREACH(n, &p->threads) {
        struct thread *t = LIST_ENTRY(n, struct thread, link);
        if (t->exited || (t->sigmask & bit) != 0) {
            continue;
        }
        if (t->park_kind == PARK_SIGSUSPEND) {
            return t;
        }
        if (best == NULL || t != active_caller) {
            best = t;
        }
    }
    return best;
}

int sig_send_proc(struct proc *p, int sig, const struct fastix_siginfo *info)
{
    if (p == NULL || p->zombie) {
        return -FASTIX_ESRCH;
    }
    if (sig == 0) {
        return 0;
    }
    if (!sig_valid(sig)) {
        return -FASTIX_EINVAL;
    }
    if (sig == FASTIX_SIGKILL) {
        if (p->pid != 1) {
            proc_exit(p, FASTIX_W_SIGNALED(FASTIX_SIGKILL));
        }
        return 0;
    }
    if (sig == FASTIX_SIGSTOP) {
        stop_process(p, FASTIX_SIGSTOP);
        return 0;
    }
    if (sig == FASTIX_SIGCONT) {
        continue_process(p);
        if (p->sigact[FASTIX_SIGCONT].handler == FASTIX_SIG_DFL ||
            p->sigact[FASTIX_SIGCONT].handler == FASTIX_SIG_IGN) {
            return 0;
        }
    }
    if (p->sigact[sig].handler == FASTIX_SIG_IGN) {
        return 0;
    }
    if (p->sigact[sig].handler == FASTIX_SIG_DFL && sig_default_action(sig) == DEF_IGN) {
        /* If no thread blocks this default-ignored signal (e.g. SIGCHLD), drop it directly. */
        bool blocked_anywhere = false;
        struct list_node *n;
        LIST_FOREACH(n, &p->threads) {
            struct thread *t = LIST_ENTRY(n, struct thread, link);
            if (!t->exited && (t->sigmask & fastix_sig_bit(sig)) != 0) {
                blocked_anywhere = true;
                break;
            }
        }
        if (!blocked_anywhere) {
            return 0;
        }
    }
    int err = sigq_enqueue(&p->sigq, &p->pending_mask, &p->sigq_count, sig, info);
    if (err != 0) {
        return err;
    }
    if (wake_sigwaiter(p, NULL, sig)) {
        return 0;
    }
    struct thread *target = pick_target_thread(p, sig);
    if (target != NULL) {
        kick_thread(target);
    }
    return 0;
}

int sig_send_thread(struct thread *t, int sig, const struct fastix_siginfo *info)
{
    if (t == NULL || t->exited || t->proc->zombie) {
        return -FASTIX_ESRCH;
    }
    if (sig == 0) {
        return 0;
    }
    if (!sig_valid(sig)) {
        return -FASTIX_EINVAL;
    }
    if (sig == FASTIX_SIGKILL || sig == FASTIX_SIGSTOP || sig == FASTIX_SIGCONT) {
        return sig_send_proc(t->proc, sig, info);
    }
    if (t->proc->sigact[sig].handler == FASTIX_SIG_IGN) {
        return 0;
    }
    int err = sigq_enqueue(&t->sigq, &t->pending_mask, &t->sigq_count, sig, info);
    if (err != 0) {
        return err;
    }
    if (wake_sigwaiter(t->proc, t, sig)) {
        return 0;
    }
    kick_thread(t);
    return 0;
}

void sig_send_child(struct proc *parent, const struct proc *child, int cld_code, int cld_status)
{
    if (parent == NULL || parent->zombie) {
        return;
    }
    if ((cld_code == FASTIX_CLD_STOPPED || cld_code == FASTIX_CLD_CONTINUED) &&
        (parent->sigact[FASTIX_SIGCHLD].flags & FASTIX_SA_NOCLDSTOP) != 0) {
        return;
    }
    struct fastix_siginfo info = {
        .si_signo = FASTIX_SIGCHLD,
        .si_code = cld_code,
        .si_pid = child->pid,
        .si_uid = child->uid,
        .si_status = cld_status,
    };
    (void)sig_send_proc(parent, FASTIX_SIGCHLD, &info);
}

bool sig_deliver_fault(struct thread *t, int sig, int code, uint64_t fault_addr)
{
    struct proc *p = t->proc;
    uint64_t handler = p->sigact[sig].handler;
    if (handler == FASTIX_SIG_DFL || handler == FASTIX_SIG_IGN ||
        (t->sigmask & fastix_sig_bit(sig)) != 0) {
        return false;
    }
    seL4_UserContext regs;
    if (seL4_TCB_ReadRegisters(t->tcb.cap, 1, 0, ARCH_REGS_COUNT, &regs) != seL4_NoError) {
        return false;
    }
    struct fastix_siginfo info = {
        .si_signo = sig,
        .si_code = code,
        .si_addr = fault_addr,
    };
    return push_sigframe_and_run(t, sig, &info, &regs, t->sigmask);
}

bool sig_check_on_return(struct thread *t, unsigned nret, const uint64_t *ret)
{
    if (t == NULL || t->exited || t->proc->zombie || t->proc->stopped) {
        return false;
    }
    if (sig_next_deliverable(t) == 0) {
        return false;
    }
    seL4_UserContext regs;
    if (seL4_TCB_ReadRegisters(t->tcb.cap, 1, 0, ARCH_REGS_COUNT, &regs) != seL4_NoError) {
        return false;
    }
    arch_regs_return_from_call_n(&regs, nret, ret);
    if (nret > 4) {
        for (unsigned i = 4; i < nret; i++) {
            uint64_t off = offsetof(seL4_IPCBuffer, msg) + i * sizeof(seL4_Word);
            (void)frame_write(t->ipcbuf, off, &ret[i], sizeof(uint64_t), false);
        }
    }
    if (sig_deliver_pending(t, &regs)) {
        return true;
    }
    /* All pending signals were ignored: resume thread with its syscall result. */
    (void)proc_thread_start(t, &regs);
    return true;
}

