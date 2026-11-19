/*
 * timer.h - high-resolution sleeps, setitimer/alarm, and POSIX interval timers.
 * Driven by a lightweight MCS periodic ticker that runs only while at least one
 * timer or sleeper is armed, signaling a notification bound to procd's main thread.
 */
#ifndef PROCD_TIMER_H
#define PROCD_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include <libos/compiler.h>
#include <sel4/sel4.h>

#include "proc.h"

/* Sets up the bound notification and suspended MCS ticker thread. */
LIBOS_MUST_CHECK int timer_module_init(seL4_CPtr sched_control);

/* Called by server.c when a timer notification arrives; fires all due timers/sleepers. */
void timer_tick(void);

/* Reference-count external armed deadlines (e.g. futex timeouts) on the ticker. */
void timer_external_inc(void);
void timer_external_dec(void);

/* Lifecycle hooks. */
void timer_proc_init(struct proc *p);
void timer_proc_cleanup(struct proc *p);
void timer_cancel_sleep(struct thread *t);

/* Syscall handlers. */
int timer_sys_nanosleep(struct thread *t, int clockid, uint64_t flags, uint64_t req_u,
                        uint64_t rem_u);
int timer_sys_setitimer(struct proc *p, int which, uint64_t new_u, uint64_t old_u);
int timer_sys_getitimer(struct proc *p, int which, uint64_t cur_u);
int timer_sys_create(struct proc *p, int clockid, uint64_t sevp_u, uint64_t timerid_u);
int timer_sys_settime(struct proc *p, int32_t id, uint64_t flags, uint64_t new_u, uint64_t old_u);
int timer_sys_gettime(struct proc *p, int32_t id, uint64_t cur_u);
int timer_sys_getoverrun(struct proc *p, int32_t id, int32_t *out);
int timer_sys_delete(struct proc *p, int32_t id);

#endif /* PROCD_TIMER_H */
