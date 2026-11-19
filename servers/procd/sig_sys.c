/* sig_sys.c - signal system calls (split from signal.c). */
#include <stddef.h>
#include <stdint.h>
#include <arch/regs.h>
#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/cspace.h>
#include <fastix/errno.h>
#include <fastix/signal_abi.h>
#include <fastix/time_abi.h>
#include <fastix/vspace.h>
#include <libos/assert.h>
#include <libos/mem.h>
#include <libos/slab.h>
#include "futex.h"
#include "heap.h"
#include "pipe.h"
#include "sig_internal.h"
#include "signal.h"
#include "vdso.h"
#include "timer.h"

/* ---- syscalls ---- */

int sig_sys_action(struct proc *p, int sig, uint64_t act_u, uint64_t oldact_u)
{
    if (!sig_valid(sig)) {
        return -FASTIX_EINVAL;
    }
    if (act_u != 0 && (sig == FASTIX_SIGKILL || sig == FASTIX_SIGSTOP)) {
        return -FASTIX_EINVAL;
    }
    if (oldact_u != 0) {
        int err = vms_write_user(p->vs, oldact_u, &p->sigact[sig], sizeof(struct fastix_sigaction));
        if (err != 0) {
            return err;
        }
    }
    if (act_u != 0) {
        struct fastix_sigaction act;
        int err = vms_read(p->vs, act_u, &act, sizeof(act));
        if (err != 0) {
            return err;
        }
        act.mask &= ~UNCATCHABLE_MASK;
        p->sigact[sig] = act;
        if (act.handler == FASTIX_SIG_IGN ||
            (act.handler == FASTIX_SIG_DFL && sig_default_action(sig) == DEF_IGN)) {
            uint64_t bit = fastix_sig_bit(sig);
            sig_discard_mask(&p->sigq, &p->pending_mask, &p->sigq_count, bit);
            struct list_node *n;
            LIST_FOREACH(n, &p->threads) {
                struct thread *t = LIST_ENTRY(n, struct thread, link);
                sig_discard_mask(&t->sigq, &t->pending_mask, &t->sigq_count, bit);
            }
        }
    }
    return 0;
}

int sig_sys_procmask(struct thread *t, int how, uint64_t set_u, uint64_t oldset_u)
{
    if (oldset_u != 0) {
        int err = vms_write_user(t->proc->vs, oldset_u, &t->sigmask, sizeof(uint64_t));
        if (err != 0) {
            return err;
        }
    }
    if (set_u != 0) {
        uint64_t set = 0;
        int err = vms_read(t->proc->vs, set_u, &set, sizeof(set));
        if (err != 0) {
            return err;
        }
        set &= ~UNCATCHABLE_MASK;
        switch (how) {
        case FASTIX_SIG_BLOCK:
            t->sigmask |= set;
            break;
        case FASTIX_SIG_UNBLOCK:
            t->sigmask &= ~set;
            break;
        case FASTIX_SIG_SETMASK:
            t->sigmask = set;
            break;
        default:
            return -FASTIX_EINVAL;
        }
    }
    return 0;
}

int sig_sys_pending(struct thread *t, uint64_t set_u)
{
    uint64_t pend = t->pending_mask | t->proc->pending_mask;
    return vms_write_user(t->proc->vs, set_u, &pend, sizeof(pend));
}

int sig_sys_altstack(struct thread *t, uint64_t ss_u, uint64_t oldss_u)
{
    seL4_UserContext regs = { 0 };
    (void)seL4_TCB_ReadRegisters(t->tcb.cap, 0, 0, ARCH_REGS_COUNT, &regs);
    bool currently_on = sig_on_altstack(t, regs.sp);

    if (oldss_u != 0) {
        struct fastix_sigaltstack old = t->altstack;
        if (currently_on) {
            old.ss_flags |= FASTIX_SS_ONSTACK;
        }
        int err = vms_write_user(t->proc->vs, oldss_u, &old, sizeof(old));
        if (err != 0) {
            return err;
        }
    }
    if (ss_u != 0) {
        if (currently_on) {
            return -FASTIX_EPERM;
        }
        struct fastix_sigaltstack ss;
        int err = vms_read(t->proc->vs, ss_u, &ss, sizeof(ss));
        if (err != 0) {
            return err;
        }
        if ((ss.ss_flags & ~(FASTIX_SS_DISABLE | FASTIX_SS_ONSTACK)) != 0) {
            return -FASTIX_EINVAL;
        }
        if (ss.ss_flags & FASTIX_SS_DISABLE) {
            t->altstack = (struct fastix_sigaltstack){ .ss_flags = FASTIX_SS_DISABLE };
        } else {
            if (ss.ss_size < FASTIX_MINSIGSTKSZ) {
                return -FASTIX_ENOMEM;
            }
            t->altstack = (struct fastix_sigaltstack){
                .ss_sp = ss.ss_sp,
                .ss_flags = 0,
                .ss_size = ss.ss_size,
            };
        }
    }
    return 0;
}

static bool can_signal(const struct proc *caller, const struct proc *target, int sig)
{
    if (target->is_server) {
        return false; /* system servers (vfsd) cannot be signalled by user processes */
    }
    if (caller->euid == 0 || caller->uid == 0) {
        return true;
    }
    if (sig == FASTIX_SIGCONT && caller->sid == target->sid) {
        return true;
    }
    return caller->uid == target->uid || caller->uid == target->suid ||
           caller->euid == target->uid || caller->euid == target->suid;
}

int sig_sys_kill(struct proc *caller, int32_t pid, int sig)
{
    if (sig < 0 || sig >= FASTIX_NSIG) {
        return -FASTIX_EINVAL;
    }
    struct fastix_siginfo info = {
        .si_signo = sig,
        .si_code = FASTIX_SI_USER,
        .si_pid = caller->pid,
        .si_uid = caller->uid,
    };
    if (pid > 0) {
        struct proc *target = proc_get((uint32_t)pid);
        if (target == NULL || target->zombie) {
            return -FASTIX_ESRCH;
        }
        if (!can_signal(caller, target, sig)) {
            return -FASTIX_EPERM;
        }
        return sig_send_proc(target, sig, &info);
    }
    /* pid == 0 (caller's pgid), pid == -1 (all except PID 1), pid < -1 (pgid == -pid) */
    uint32_t pgid = pid == 0 ? caller->pgid : (uint32_t)(-pid);
    bool matched = false, permitted = false;
    for (unsigned i = 0; i < FASTIX_MAX_PROCS; i++) {
        struct proc *target = proc_table_at(i);
        if (target == NULL || target->zombie) {
            continue;
        }
        if (pid == -1) {
            if (target->pid == 1 || target == caller) {
                continue;
            }
        } else if (target->pgid != pgid) {
            continue;
        }
        matched = true;
        if (!can_signal(caller, target, sig)) {
            continue;
        }
        permitted = true;
        (void)sig_send_proc(target, sig, &info);
    }
    if (!matched) {
        return -FASTIX_ESRCH;
    }
    return permitted ? 0 : -FASTIX_EPERM;
}

int sig_sys_tkill(struct proc *caller, uint32_t tid, int sig)
{
    if (sig < 0 || sig >= FASTIX_NSIG) {
        return -FASTIX_EINVAL;
    }
    struct thread *target = proc_find_thread_global(tid);
    if (target == NULL) {
        return -FASTIX_ESRCH;
    }
    if (!can_signal(caller, target->proc, sig)) {
        return -FASTIX_EPERM;
    }
    struct fastix_siginfo info = {
        .si_signo = sig,
        .si_code = FASTIX_SI_TKILL,
        .si_pid = caller->pid,
        .si_uid = caller->uid,
    };
    return sig_send_thread(target, sig, &info);
}

int sig_sys_tgkill(struct proc *caller, uint32_t tgid, uint32_t tid, int sig)
{
    if (sig < 0 || sig >= FASTIX_NSIG) {
        return -FASTIX_EINVAL;
    }
    struct proc *p = proc_get(tgid);
    if (p == NULL || p->zombie) {
        return -FASTIX_ESRCH;
    }
    struct thread *target = proc_find_thread(p, tid);
    if (target == NULL) {
        return -FASTIX_ESRCH;
    }
    if (!can_signal(caller, p, sig)) {
        return -FASTIX_EPERM;
    }
    struct fastix_siginfo info = {
        .si_signo = sig,
        .si_code = FASTIX_SI_TKILL,
        .si_pid = caller->pid,
        .si_uid = caller->uid,
    };
    return sig_send_thread(target, sig, &info);
}

int sig_sys_queueinfo(struct proc *caller, uint32_t pid, int sig, uint64_t uinfo_u)
{
    if (sig < 0 || sig >= FASTIX_NSIG) {
        return -FASTIX_EINVAL;
    }
    struct proc *target = proc_get(pid);
    if (target == NULL || target->zombie) {
        return -FASTIX_ESRCH;
    }
    if (!can_signal(caller, target, sig)) {
        return -FASTIX_EPERM;
    }
    struct fastix_siginfo info = { 0 };
    if (uinfo_u != 0) {
        int err = vms_read(caller->vs, uinfo_u, &info, sizeof(info));
        if (err != 0) {
            return err;
        }
    } else {
        info.si_code = FASTIX_SI_QUEUE;
    }
    info.si_signo = sig;
    info.si_pid = caller->pid;
    info.si_uid = caller->uid;
    return sig_send_proc(target, sig, &info);
}

int sig_sys_suspend(struct thread *t, uint64_t mask_u)
{
    uint64_t mask = 0;
    int err = vms_read(t->proc->vs, mask_u, &mask, sizeof(mask));
    if (err != 0) {
        return err;
    }
    t->saved_sigmask = t->sigmask;
    t->has_saved_sigmask = true;
    t->sigmask = mask & ~UNCATCHABLE_MASK;

    if (sig_next_deliverable(t) != 0) {
        /* A deliverable signal is already pending: return EINTR so sig_check_on_return runs it. */
        return -FASTIX_EINTR;
    }
    return 1; /* park in PARK_SIGSUSPEND */
}

void sig_sys_return(struct thread *t, uint64_t frame_sp)
{
    struct proc *p = t->proc;
    struct fastix_sigframe frame;
    if (vms_read(p->vs, frame_sp, &frame, sizeof(frame)) != 0 ||
        frame.magic != FASTIX_SIGFRAME_MAGIC) {
        proc_exit(p, FASTIX_W_SIGNALED(FASTIX_SIGSEGV));
        return;
    }

    t->sigmask = frame.uc.uc_sigmask & ~UNCATCHABLE_MASK;
    t->has_saved_sigmask = false;

    seL4_UserContext regs = { 0 };
    arch_regs_from_sigctx(&regs, &frame.uc.uc_mcontext);

    /* Suspend to cancel BlockedOnReply from rt_sigreturn before restoring context. */
    (void)seL4_TCB_Suspend(t->tcb.cap);
    if (!sig_deliver_pending(t, &regs)) {
        (void)proc_thread_start(t, &regs);
    }
}

#define NS_PER_SEC 1000000000ULL

int sig_sys_timedwait(struct thread *t, uint64_t set_u, uint64_t info_u, uint64_t timeout_u,
                      uint64_t setsize)
{
    uint64_t set = 0;
    struct fastix_timespec ts = { 0, 0 };

    if (setsize != sizeof(set)) {
        return -FASTIX_EINVAL;
    }
    int err = vms_read(t->proc->vs, set_u, &set, sizeof(set));
    if (err == 0 && timeout_u != 0) {
        err = vms_read(t->proc->vs, timeout_u, &ts, sizeof(ts));
    }
    if (err != 0) {
        return err;
    }
    if (timeout_u != 0 && (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= (int64_t)NS_PER_SEC)) {
        return -FASTIX_EINVAL;
    }
    set &= ~UNCATCHABLE_MASK;
    int sig = sig_take_waited(t, set, info_u);
    if (sig != 0) {
        return sig;
    }
    if (sig_next_deliverable(t) != 0) {
        return -FASTIX_EINTR; /* another signal is deliverable: its handler runs first */
    }
    uint64_t timeout_ns = (uint64_t)ts.tv_sec > (UINT64_MAX - (uint64_t)ts.tv_nsec) / NS_PER_SEC
                              ? UINT64_MAX
                              : (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
    if (timeout_u != 0 && timeout_ns == 0) {
        return -FASTIX_EAGAIN; /* poll */
    }
    t->sigwait_set = set;
    t->sigwait_info_ptr = info_u;
    t->sigwait_timed = timeout_u != 0;
    if (t->sigwait_timed) {
        uint64_t now = vdso_mono_ns();
        t->sleep_clock = FASTIX_CLOCK_MONOTONIC;
        t->sleep_deadline_ns = timeout_ns > UINT64_MAX - now ? UINT64_MAX : now + timeout_ns;
        t->sleep_rem_ptr = 0;
        t->sleep_abstime = true;
    }
    return SIG_TIMEDWAIT_PARK;
}

void sig_timedwait_arm(struct thread *t)
{
    if (t->sigwait_timed) {
        timer_arm_park_timeout(t);
    }
}
