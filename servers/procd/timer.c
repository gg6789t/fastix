/* timer.c - sleeps, setitimer, and POSIX interval timers (see timer.h). */
#include <stdbool.h>
#include <stdint.h>

#include <arch/regs.h>
#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/cspace.h>
#include <fastix/errno.h>
#include <fastix/signal_abi.h>
#include <fastix/time_abi.h>
#include <libos/assert.h>
#include <libos/slab.h>
#include <sel4/sel4.h>

#include "cspace.h"
#include "futex.h"
#include "heap.h"
#include "kobj.h"
#include "signal.h"
#include "ticker.h"
#include "timer.h"
#include "vdso.h"

#define TICKER_PERIOD_US 1000u /* 1 ms tick while any timer/sleeper is armed */
#define TICKER_BUDGET_US 200u
#define TICKER_PRIO      (FASTIX_DEFAULT_PRIO + 1)

#define TICKER_STACK_BYTES 4096

static uint64_t ticker_stack[TICKER_STACK_BYTES / sizeof(uint64_t)] __attribute__((aligned(16)));

struct posix_timer {
    struct list_node link; /* in proc->posix_timers */
    int32_t id;
    int clockid;
    int notify;
    int signo;
    uint64_t sival;
    uint64_t deadline_ns; /* 0 = disarmed */
    uint64_t interval_ns;
    int32_t overrun;
};

static struct slab_cache ptimer_cache;
static struct list_node sleepers;
static uint32_t armed_count;
static bool ticker_running;

static struct kobj timer_ntfn;
static seL4_CPtr timer_ntfn_badged;
static struct kobj ticker_tcb;
static struct kobj ticker_sc;

static void ticker_sync(void)
{
    if (armed_count > 0 && !ticker_running) {
        (void)seL4_TCB_Resume(ticker_tcb.cap);
        ticker_running = true;
    } else if (armed_count == 0 && ticker_running) {
        (void)seL4_TCB_Suspend(ticker_tcb.cap);
        ticker_running = false;
    }
}

static void armed_inc(void)
{
    armed_count++;
    ticker_sync();
}

static void armed_dec(void)
{
    libos_assert(armed_count > 0);
    armed_count--;
    ticker_sync();
}

void timer_external_inc(void)
{
    armed_inc();
}

void timer_external_dec(void)
{
    armed_dec();
}

int timer_module_init(seL4_CPtr sched_control)
{
    slab_init(&ptimer_cache, "posix_timer", sizeof(struct posix_timer), heap_provider());
    list_init(&sleepers);

    int err;
    if ((err = kobj_alloc(seL4_NotificationObject, 0, &timer_ntfn)) != 0 ||
        (err = kobj_slot_get(&timer_ntfn_badged)) != 0) {
        return err;
    }
    uint64_t badge = fastix_badge_make(FASTIX_BADGE_NOTIFY, 0, 0, 1);
    err = kobj_sel4_err(seL4_CNode_Mint(cspace_root(), timer_ntfn_badged, seL4_WordBits,
                                        cspace_root(), timer_ntfn.cap, seL4_WordBits,
                                        seL4_AllRights, badge));
    if (err != 0) {
        return err;
    }
    err = kobj_sel4_err(seL4_TCB_BindNotification(seL4_CapInitThreadTCB, timer_ntfn.cap));
    if (err != 0) {
        return err;
    }

    if ((err = kobj_alloc(seL4_TCBObject, 0, &ticker_tcb)) != 0 ||
        (err = kobj_alloc(seL4_SchedContextObject, seL4_MinSchedContextBits, &ticker_sc)) != 0) {
        return err;
    }
    err = kobj_sel4_err(seL4_TCB_Configure(ticker_tcb.cap, cspace_root(), 0,
                                           seL4_CapInitThreadVSpace, 0, 0, seL4_CapNull));
    if (err != 0) {
        return err;
    }
    err = kobj_sel4_err(seL4_SchedControl_ConfigureFlags(sched_control, ticker_sc.cap,
                                                         TICKER_BUDGET_US, TICKER_PERIOD_US, 0, 0,
                                                         0));
    if (err != 0) {
        return err;
    }
    err = kobj_sel4_err(seL4_TCB_SetSchedParams(ticker_tcb.cap, seL4_CapInitThreadTCB, 0,
                                                TICKER_PRIO, ticker_sc.cap, seL4_CapNull));
    if (err != 0) {
        return err;
    }
    seL4_UserContext regs;
    arch_regs_init_entry(&regs, (uint64_t)(uintptr_t)&ticker_main,
                         (uint64_t)(uintptr_t)(ticker_stack + sizeof(ticker_stack) / sizeof(ticker_stack[0])),
                         timer_ntfn_badged);
    /* Write registers without resuming (resume_target = 0); starts on first armed timer. */
    return kobj_sel4_err(seL4_TCB_WriteRegisters(ticker_tcb.cap, 0, 0, ARCH_REGS_COUNT, &regs));
}

/* ---- time conversions ---- */

static uint64_t clock_now_ns(int clockid)
{
    uint64_t mono = vdso_mono_ns();
    if (clockid == FASTIX_CLOCK_REALTIME) {
        return mono + vdso_realtime_offset_ns();
    }
    return mono;
}

static bool valid_clock(int clockid)
{
    return clockid == FASTIX_CLOCK_REALTIME || clockid == FASTIX_CLOCK_MONOTONIC ||
           clockid == FASTIX_CLOCK_BOOTTIME;
}

static bool ts_valid(const struct fastix_timespec *ts)
{
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && (uint64_t)ts->tv_nsec < FASTIX_NSEC_PER_SEC;
}

static uint64_t ts_to_ns(const struct fastix_timespec *ts)
{
    uint64_t sec = (uint64_t)ts->tv_sec;
    uint64_t nsec = (uint64_t)ts->tv_nsec;
    if (sec > (UINT64_MAX - nsec) / FASTIX_NSEC_PER_SEC) {
        return UINT64_MAX;
    }
    return sec * FASTIX_NSEC_PER_SEC + nsec;
}

static struct fastix_timespec ns_to_ts(uint64_t ns)
{
    return (struct fastix_timespec){
        .tv_sec = (int64_t)(ns / FASTIX_NSEC_PER_SEC),
        .tv_nsec = (int64_t)(ns % FASTIX_NSEC_PER_SEC),
    };
}

static bool tv_valid(const struct fastix_timeval *tv)
{
    return tv->tv_sec >= 0 && tv->tv_usec >= 0 && (uint64_t)tv->tv_usec < FASTIX_USEC_PER_SEC;
}

static uint64_t tv_to_ns(const struct fastix_timeval *tv)
{
    uint64_t sec = (uint64_t)tv->tv_sec;
    uint64_t usec = (uint64_t)tv->tv_usec;
    if (sec > (UINT64_MAX - usec * 1000ULL) / FASTIX_NSEC_PER_SEC) {
        return UINT64_MAX;
    }
    return sec * FASTIX_NSEC_PER_SEC + usec * 1000ULL;
}

static struct fastix_timeval ns_to_tv(uint64_t ns)
{
    uint64_t us = (ns + 999ULL) / 1000ULL;
    return (struct fastix_timeval){
        .tv_sec = (int64_t)(us / FASTIX_USEC_PER_SEC),
        .tv_usec = (int64_t)(us % FASTIX_USEC_PER_SEC),
    };
}

/* ---- lifecycle ---- */

void timer_proc_init(struct proc *p)
{
    p->itimer_deadline_ns = 0;
    p->itimer_interval_ns = 0;
    list_init(&p->posix_timers);
}

void timer_proc_cleanup(struct proc *p)
{
    if (p->itimer_deadline_ns != 0) {
        p->itimer_deadline_ns = 0;
        p->itimer_interval_ns = 0;
        armed_dec();
    }
    struct list_node *n;
    while ((n = list_pop(&p->posix_timers)) != NULL) {
        struct posix_timer *pt = LIST_ENTRY(n, struct posix_timer, link);
        if (pt->deadline_ns != 0) {
            armed_dec();
        }
        slab_free(&ptimer_cache, pt);
    }
}

void timer_cancel_sleep(struct thread *t)
{
    if (t->park_kind == PARK_SIGWAIT && t->sigwait_timed) {
        t->sigwait_timed = false;
        list_remove(&t->sleep_link);
        armed_dec();
        return;
    }
    if (t->park_kind != PARK_SLEEP) {
        return;
    }
    list_remove(&t->sleep_link);
    armed_dec();

    if (!t->sleep_abstime && t->sleep_rem_ptr != 0) {
        uint64_t now = clock_now_ns(t->sleep_clock);
        uint64_t rem_ns = t->sleep_deadline_ns > now ? t->sleep_deadline_ns - now : 0;
        struct fastix_timespec rem = ns_to_ts(rem_ns);
        (void)vms_write_user(t->proc->vs, t->sleep_rem_ptr, &rem, sizeof(rem));
    }
}

void timer_arm_park_timeout(struct thread *t)
{
    list_push_back(&sleepers, &t->sleep_link);
    armed_inc();
}

/* ---- tick processing ---- */

static void tick_sleepers(void)
{
    struct list_node *n, *tmp;
    LIST_FOREACH_SAFE(n, tmp, &sleepers) {
        struct thread *t = LIST_ENTRY(n, struct thread, sleep_link);
        uint64_t now = clock_now_ns(t->sleep_clock);
        if (now < t->sleep_deadline_ns) {
            continue;
        }
        list_remove(n);
        armed_dec();
        /* A sleep ends normally; a timed rt_sigtimedwait ends with EAGAIN. */
        uint64_t ret = t->park_kind == PARK_SIGWAIT ? (uint64_t)(int64_t)-FASTIX_EAGAIN : 0;
        t->sigwait_timed = false;
        struct reply_obj *rep = t->park_reply;
        t->park_reply = NULL;
        t->park_kind = PARK_NONE;
        uint64_t mr[1] = { ret };
        reply_send(rep, 1, mr);
    }
}

static void tick_proc_timers(struct proc *p)
{
    if (p->itimer_deadline_ns != 0) {
        uint64_t now = vdso_mono_ns();
        if (now >= p->itimer_deadline_ns) {
            if (p->itimer_interval_ns != 0) {
                uint64_t step = (now - p->itimer_deadline_ns) / p->itimer_interval_ns + 1;
                p->itimer_deadline_ns += step * p->itimer_interval_ns;
            } else {
                p->itimer_deadline_ns = 0;
                armed_dec();
            }
            struct fastix_siginfo info = {
                .si_signo = FASTIX_SIGALRM,
                .si_code = FASTIX_SI_TIMER,
            };
            (void)sig_send_proc(p, FASTIX_SIGALRM, &info);
            if (p->zombie) {
                return;
            }
        }
    }

    struct list_node *n, *tmp;
    LIST_FOREACH_SAFE(n, tmp, &p->posix_timers) {
        struct posix_timer *pt = LIST_ENTRY(n, struct posix_timer, link);
        if (pt->deadline_ns == 0) {
            continue;
        }
        uint64_t now = clock_now_ns(pt->clockid);
        if (now < pt->deadline_ns) {
            continue;
        }
        if (pt->interval_ns != 0) {
            uint64_t step = (now - pt->deadline_ns) / pt->interval_ns + 1;
            pt->overrun = (int32_t)(step - 1);
            pt->deadline_ns += step * pt->interval_ns;
        } else {
            pt->overrun = 0;
            pt->deadline_ns = 0;
            armed_dec();
        }
        if (pt->notify == FASTIX_SIGEV_SIGNAL) {
            struct fastix_siginfo info = {
                .si_signo = pt->signo,
                .si_code = FASTIX_SI_TIMER,
                .si_timerid = pt->id,
                .si_value = pt->sival,
            };
            (void)sig_send_proc(p, pt->signo, &info);
            if (p->zombie) {
                return;
            }
        }
    }
}

void timer_tick(void)
{
    tick_sleepers();
    futex_tick();
    for (unsigned i = 0; i < FASTIX_MAX_PROCS; i++) {
        struct proc *p = proc_table_at(i);
        if (p != NULL && !p->zombie) {
            tick_proc_timers(p);
        }
    }
}

/* ---- syscalls ---- */

int timer_sys_nanosleep(struct thread *t, int clockid, uint64_t flags, uint64_t req_u,
                        uint64_t rem_u)
{
    if (!valid_clock(clockid) || (flags & ~(uint64_t)FASTIX_TIMER_ABSTIME) != 0) {
        return -FASTIX_EINVAL;
    }
    struct fastix_timespec req;
    int err = vms_read(t->proc->vs, req_u, &req, sizeof(req));
    if (err != 0) {
        return err;
    }
    if (!ts_valid(&req)) {
        return -FASTIX_EINVAL;
    }
    uint64_t now = clock_now_ns(clockid);
    uint64_t val = ts_to_ns(&req);
    bool abstime = (flags & FASTIX_TIMER_ABSTIME) != 0;
    uint64_t deadline = abstime ? val : (val > UINT64_MAX - now ? UINT64_MAX : now + val);

    if (deadline <= now) {
        return 0; /* already expired */
    }
    t->sleep_clock = clockid;
    t->sleep_deadline_ns = deadline;
    t->sleep_rem_ptr = rem_u;
    t->sleep_abstime = abstime;
    list_push_back(&sleepers, &t->sleep_link);
    armed_inc();
    return 1; /* park in PARK_SLEEP */
}

int timer_sys_setitimer(struct proc *p, int which, uint64_t new_u, uint64_t old_u)
{
    if (which != FASTIX_ITIMER_REAL) {
        return -FASTIX_EINVAL;
    }
    struct fastix_itimerval nv;
    int err = vms_read(p->vs, new_u, &nv, sizeof(nv));
    if (err != 0) {
        return err;
    }
    if (!tv_valid(&nv.it_value) || !tv_valid(&nv.it_interval)) {
        return -FASTIX_EINVAL;
    }
    if (old_u != 0) {
        err = timer_sys_getitimer(p, which, old_u);
        if (err != 0) {
            return err;
        }
    }
    bool was_armed = p->itimer_deadline_ns != 0;
    uint64_t val_ns = tv_to_ns(&nv.it_value);
    if (val_ns == 0) {
        p->itimer_deadline_ns = 0;
        p->itimer_interval_ns = 0;
        if (was_armed) {
            armed_dec();
        }
    } else {
        p->itimer_deadline_ns = vdso_mono_ns() + val_ns;
        p->itimer_interval_ns = tv_to_ns(&nv.it_interval);
        if (!was_armed) {
            armed_inc();
        }
    }
    return 0;
}

int timer_sys_getitimer(struct proc *p, int which, uint64_t cur_u)
{
    if (which != FASTIX_ITIMER_REAL) {
        return -FASTIX_EINVAL;
    }
    struct fastix_itimerval cur = { 0 };
    if (p->itimer_deadline_ns != 0) {
        uint64_t now = vdso_mono_ns();
        uint64_t rem = p->itimer_deadline_ns > now ? p->itimer_deadline_ns - now : 1;
        cur.it_value = ns_to_tv(rem);
        cur.it_interval = ns_to_tv(p->itimer_interval_ns);
    }
    return vms_write_user(p->vs, cur_u, &cur, sizeof(cur));
}

static struct posix_timer *find_ptimer(struct proc *p, int32_t id)
{
    struct list_node *n;
    LIST_FOREACH(n, &p->posix_timers) {
        struct posix_timer *pt = LIST_ENTRY(n, struct posix_timer, link);
        if (pt->id == id) {
            return pt;
        }
    }
    return NULL;
}

int timer_sys_create(struct proc *p, int clockid, uint64_t sevp_u, uint64_t timerid_u)
{
    if (!valid_clock(clockid)) {
        return -FASTIX_EINVAL;
    }
    struct fastix_sigevent sev = {
        .sigev_signo = FASTIX_SIGALRM,
        .sigev_notify = FASTIX_SIGEV_SIGNAL,
    };
    if (sevp_u != 0) {
        int err = vms_read(p->vs, sevp_u, &sev, sizeof(sev));
        if (err != 0) {
            return err;
        }
    }
    if (sev.sigev_notify != FASTIX_SIGEV_SIGNAL && sev.sigev_notify != FASTIX_SIGEV_NONE) {
        return -FASTIX_EINVAL;
    }
    if (sev.sigev_notify == FASTIX_SIGEV_SIGNAL &&
        (sev.sigev_signo < 1 || sev.sigev_signo >= FASTIX_NSIG)) {
        return -FASTIX_EINVAL;
    }
    int32_t id = -1;
    for (int32_t cand = 0; cand < FASTIX_MAX_TIMERS_PER_PROC; cand++) {
        if (find_ptimer(p, cand) == NULL) {
            id = cand;
            break;
        }
    }
    if (id < 0) {
        return -FASTIX_EAGAIN;
    }
    struct posix_timer *pt = slab_alloc(&ptimer_cache);
    if (pt == NULL) {
        return -FASTIX_ENOMEM;
    }
    pt->id = id;
    pt->clockid = clockid;
    pt->notify = sev.sigev_notify;
    pt->signo = sev.sigev_signo;
    pt->sival = sev.sigev_value;

    int err = vms_write_user(p->vs, timerid_u, &id, sizeof(id));
    if (err != 0) {
        slab_free(&ptimer_cache, pt);
        return err;
    }
    list_push_back(&p->posix_timers, &pt->link);
    return 0;
}

int timer_sys_settime(struct proc *p, int32_t id, uint64_t flags, uint64_t new_u, uint64_t old_u)
{
    if ((flags & ~(uint64_t)FASTIX_TIMER_ABSTIME) != 0) {
        return -FASTIX_EINVAL;
    }
    struct posix_timer *pt = find_ptimer(p, id);
    if (pt == NULL) {
        return -FASTIX_EINVAL;
    }
    struct fastix_itimerspec nv;
    int err = vms_read(p->vs, new_u, &nv, sizeof(nv));
    if (err != 0) {
        return err;
    }
    if (!ts_valid(&nv.it_value) || !ts_valid(&nv.it_interval)) {
        return -FASTIX_EINVAL;
    }
    if (old_u != 0) {
        err = timer_sys_gettime(p, id, old_u);
        if (err != 0) {
            return err;
        }
    }
    bool was_armed = pt->deadline_ns != 0;
    uint64_t val_ns = ts_to_ns(&nv.it_value);
    if (val_ns == 0) {
        pt->deadline_ns = 0;
        pt->interval_ns = 0;
        if (was_armed) {
            armed_dec();
        }
    } else {
        uint64_t now = clock_now_ns(pt->clockid);
        pt->deadline_ns = (flags & FASTIX_TIMER_ABSTIME) ? val_ns : (now + val_ns);
        pt->interval_ns = ts_to_ns(&nv.it_interval);
        pt->overrun = 0;
        if (!was_armed) {
            armed_inc();
        }
    }
    return 0;
}

int timer_sys_gettime(struct proc *p, int32_t id, uint64_t cur_u)
{
    struct posix_timer *pt = find_ptimer(p, id);
    if (pt == NULL) {
        return -FASTIX_EINVAL;
    }
    struct fastix_itimerspec cur = { 0 };
    if (pt->deadline_ns != 0) {
        uint64_t now = clock_now_ns(pt->clockid);
        uint64_t rem = pt->deadline_ns > now ? pt->deadline_ns - now : 1;
        cur.it_value = ns_to_ts(rem);
        cur.it_interval = ns_to_ts(pt->interval_ns);
    }
    return vms_write_user(p->vs, cur_u, &cur, sizeof(cur));
}

int timer_sys_getoverrun(struct proc *p, int32_t id, int32_t *out)
{
    struct posix_timer *pt = find_ptimer(p, id);
    if (pt == NULL) {
        return -FASTIX_EINVAL;
    }
    *out = pt->overrun;
    return 0;
}

int timer_sys_delete(struct proc *p, int32_t id)
{
    struct posix_timer *pt = find_ptimer(p, id);
    if (pt == NULL) {
        return -FASTIX_EINVAL;
    }
    if (pt->deadline_ns != 0) {
        armed_dec();
    }
    list_remove(&pt->link);
    slab_free(&ptimer_cache, pt);
    return 0;
}
