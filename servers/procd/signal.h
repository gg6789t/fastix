/*
 * signal.h - POSIX signals: dispositions, masks, queuing (standard + RT),
 * user-stack delivery with FPU/SIMD preservation via VDSO trampoline,
 * sigaltstack, stop/continue, SIGCHLD, and EINTR / SA_RESTART.
 */
#ifndef PROCD_SIGNAL_H
#define PROCD_SIGNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <fastix/signal_abi.h>
#include <libos/compiler.h>

#include "proc.h"

void sig_module_init(void);

/* Marks which thread is currently inside server.c's dispatch() (NULL outside). */
void sig_set_active_caller(struct thread *t);

/* Lifecycle hooks called by proc.c and exec.c. */
void sig_proc_init(struct proc *p, const struct proc *parent);
void sig_thread_init(struct thread *t, const struct thread *parent_t);
void sig_proc_cleanup(struct proc *p);
void sig_thread_cleanup(struct thread *t);
void sig_on_exec(struct proc *p, struct thread *t);

/* Returns true if parent ignores SIGCHLD or set SA_NOCLDWAIT (auto-reap zombies). */
bool sig_should_autoreap(const struct proc *parent);

/* Sends a signal to a process or specific thread. Returns 0 or -errno. */
int sig_send_proc(struct proc *p, int sig, const struct fastix_siginfo *info);
int sig_send_thread(struct thread *t, int sig, const struct fastix_siginfo *info);
void sig_send_child(struct proc *parent, const struct proc *child, int cld_code, int cld_status);

/*
 * Delivers a synchronous hardware fault (SIGSEGV/SIGILL/SIGBUS/SIGFPE) to a
 * user handler if installed & unblocked. Returns true if delivered to handler,
 * false if the process must be killed by default action.
 */
bool sig_deliver_fault(struct thread *t, int sig, int code, uint64_t fault_addr);

/*
 * Called right before replying OUT_REPLY to `t`. If an unblocked pending signal
 * is ready, applies `ret[0..nret-1]` to `t`'s regs, delivers the signal (or
 * stops/kills `t`), and returns true (caller must return OUT_NONE).
 */
bool sig_check_on_return(struct thread *t, unsigned nret, const uint64_t *ret);

/* Syscall handlers. */
int sig_sys_action(struct proc *p, int sig, uint64_t act_u, uint64_t oldact_u);
int sig_sys_procmask(struct thread *t, int how, uint64_t set_u, uint64_t oldset_u);
int sig_sys_pending(struct thread *t, uint64_t set_u);
int sig_sys_altstack(struct thread *t, uint64_t ss_u, uint64_t oldss_u);
int sig_sys_kill(struct proc *caller, int32_t pid, int sig);
int sig_sys_tkill(struct proc *caller, uint32_t tid, int sig);
int sig_sys_tgkill(struct proc *caller, uint32_t tgid, uint32_t tid, int sig);
int sig_sys_queueinfo(struct proc *caller, uint32_t pid, int sig, uint64_t uinfo_u);

/*
 * rt_sigsuspend: returns 1 if caller must be parked in PARK_SIGSUSPEND,
 * or -FASTIX_EINTR (signal already pending) / -FASTIX_EFAULT.
 */
int sig_sys_suspend(struct thread *t, uint64_t mask_u);

/* rt_sigtimedwait: returns a signal number (> 0), SIG_TIMEDWAIT_PARK to park the
 * caller (then call sig_timedwait_arm once parked), or -errno. */
#define SIG_TIMEDWAIT_PARK 0
int sig_sys_timedwait(struct thread *t, uint64_t set_u, uint64_t info_u, uint64_t timeout_u,
                      uint64_t setsize);
void sig_timedwait_arm(struct thread *t);

/* rt_sigreturn: restores context & mask from user frame `frame_sp` and resumes `t`. */
void sig_sys_return(struct thread *t, uint64_t frame_sp);

#endif /* PROCD_SIGNAL_H */
