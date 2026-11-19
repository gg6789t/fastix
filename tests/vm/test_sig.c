/*
 * test_sig.c - Phase 3B behavioral tests: nested signals, sigaltstack, FPU/SIMD
 * preservation across signals, sigprocmask/sigpending/sigsuspend, realtime queued
 * signals, EINTR vs SA_RESTART, SIGSTOP/SIGCONT + WUNTRACED/WCONTINUED + SIGCHLD,
 * nanosleep/clock_nanosleep, setitimer/alarm, and POSIX timer_create/settime/delete.
 */
#include <stdint.h>

#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <fastix/mman.h>
#include <fastix/signal_abi.h>
#include <fastix/time_abi.h>
#include <fastix/vspace.h>
#include <fastix_sys/sys.h>

#include "vmtest.h"

#define PAGE FASTIX_PAGE_SIZE
#define RW   (FASTIX_PROT_READ | FASTIX_PROT_WRITE)

static int64_t sys_sigaction(int sig, const struct fastix_sigaction *act,
                             struct fastix_sigaction *old)
{
    return fastix_sys3(FASTIX_SYS_rt_sigaction, (uint64_t)sig, (uint64_t)(uintptr_t)act,
                       (uint64_t)(uintptr_t)old);
}

static int64_t sys_sigprocmask(int how, const uint64_t *set, uint64_t *old)
{
    return fastix_sys3(FASTIX_SYS_rt_sigprocmask, (uint64_t)how, (uint64_t)(uintptr_t)set,
                       (uint64_t)(uintptr_t)old);
}

static int64_t sys_kill(int32_t pid, int sig)
{
    return fastix_sys2(FASTIX_SYS_kill, (uint64_t)(int64_t)pid, (uint64_t)sig);
}

static int64_t sys_nanosleep_ms(uint64_t ms, struct fastix_timespec *rem)
{
    struct fastix_timespec req = {
        .tv_sec = (int64_t)(ms / 1000ULL),
        .tv_nsec = (int64_t)((ms % 1000ULL) * 1000000ULL),
    };
    return fastix_sys2(FASTIX_SYS_nanosleep, (uint64_t)(uintptr_t)&req, (uint64_t)(uintptr_t)rem);
}

/* ---- 1. Nested signals + sigaltstack + full FPU/SIMD preservation ---- */

static uint8_t alt_buf[16384] __attribute__((aligned(16)));
static volatile uint64_t usr1_sp, usr2_sp;
static volatile int usr1_hits, usr2_hits, usr1_fpu_ok;

static void fill_fpu(uint64_t seed)
{
    uint64_t buf[64] __attribute__((aligned(16)));
    for (int i = 0; i < 64; i++) {
        buf[i] = seed + (uint64_t)i * 0x0101010101010101ULL;
    }
    __asm__ volatile(
        "ldp q0,  q1,  [%0, #0]\n"
        "ldp q2,  q3,  [%0, #32]\n"
        "ldp q4,  q5,  [%0, #64]\n"
        "ldp q6,  q7,  [%0, #96]\n"
        "ldp q8,  q9,  [%0, #128]\n"
        "ldp q10, q11, [%0, #160]\n"
        "ldp q12, q13, [%0, #192]\n"
        "ldp q14, q15, [%0, #224]\n"
        "ldp q16, q17, [%0, #256]\n"
        "ldp q18, q19, [%0, #288]\n"
        "ldp q20, q21, [%0, #320]\n"
        "ldp q22, q23, [%0, #352]\n"
        "ldp q24, q25, [%0, #384]\n"
        "ldp q26, q27, [%0, #416]\n"
        "ldp q28, q29, [%0, #448]\n"
        "ldp q30, q31, [%0, #480]\n"
        :
        : "r"(buf)
        : "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11",
          "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
}

static int check_fpu(uint64_t seed)
{
    uint64_t buf[64] __attribute__((aligned(16)));
    __asm__ volatile(
        "stp q0,  q1,  [%0, #0]\n"
        "stp q2,  q3,  [%0, #32]\n"
        "stp q4,  q5,  [%0, #64]\n"
        "stp q6,  q7,  [%0, #96]\n"
        "stp q8,  q9,  [%0, #128]\n"
        "stp q10, q11, [%0, #160]\n"
        "stp q12, q13, [%0, #192]\n"
        "stp q14, q15, [%0, #224]\n"
        "stp q16, q17, [%0, #256]\n"
        "stp q18, q19, [%0, #288]\n"
        "stp q20, q21, [%0, #320]\n"
        "stp q22, q23, [%0, #352]\n"
        "stp q24, q25, [%0, #384]\n"
        "stp q26, q27, [%0, #416]\n"
        "stp q28, q29, [%0, #448]\n"
        "stp q30, q31, [%0, #480]\n"
        :
        : "r"(buf)
        : "memory");
    for (int i = 0; i < 64; i++) {
        if (buf[i] != seed + (uint64_t)i * 0x0101010101010101ULL) {
            return 0;
        }
    }
    return 1;
}

static void handle_usr2(int sig, struct fastix_siginfo *info, struct fastix_ucontext *uc)
{
    volatile uint8_t local;
    (void)info;
    (void)uc;
    usr2_sp = (uint64_t)(uintptr_t)&local;
    fill_fpu(0xBEEF000000000000ULL);
    if (sig == FASTIX_SIGUSR2) {
        usr2_hits++;
    }
}

static void handle_usr1(int sig, struct fastix_siginfo *info, struct fastix_ucontext *uc)
{
    volatile uint8_t local;
    (void)info;
    (void)uc;
    usr1_sp = (uint64_t)(uintptr_t)&local;
    if (sig == FASTIX_SIGUSR1) {
        usr1_hits++;
    }
    fill_fpu(0xDEAD000000000000ULL);
    /* Trigger a nested SIGUSR2 while inside SIGUSR1 on the altstack! */
    (void)sys_kill((int32_t)fastix_procinfo()->pid, FASTIX_SIGUSR2);
    /* SIGUSR2 clobbered FPU with 0xBEEF...; our 0xDEAD... must be restored! */
    usr1_fpu_ok = check_fpu(0xDEAD000000000000ULL);
}

static void t_nested_altstack_fpu(void)
{
    int ok = 1;
    uint64_t alt_lo = (uint64_t)(uintptr_t)alt_buf;
    uint64_t alt_hi = alt_lo + sizeof(alt_buf);

    /* Verify VDSO code page has valid ELF magic. */
    const uint8_t *ehdr = (const uint8_t *)(uintptr_t)FASTIX_VDSO_CODE_ADDR;
    ok &= ehdr[0] == 0x7f && ehdr[1] == 'E' && ehdr[2] == 'L' && ehdr[3] == 'F';

    struct fastix_sigaltstack ss = { .ss_sp = alt_lo, .ss_flags = 0, .ss_size = sizeof(alt_buf) };
    ok &= fastix_sys2(FASTIX_SYS_sigaltstack, (uint64_t)(uintptr_t)&ss, 0) == 0;

    struct fastix_sigaction sa1 = {
        .handler = (uint64_t)(uintptr_t)&handle_usr1,
        .flags = FASTIX_SA_ONSTACK | FASTIX_SA_SIGINFO,
    };
    struct fastix_sigaction sa2 = {
        .handler = (uint64_t)(uintptr_t)&handle_usr2,
        .flags = FASTIX_SA_ONSTACK | FASTIX_SA_SIGINFO,
    };
    ok &= sys_sigaction(FASTIX_SIGUSR1, &sa1, 0) == 0;
    ok &= sys_sigaction(FASTIX_SIGUSR2, &sa2, 0) == 0;

    usr1_hits = 0;
    usr2_hits = 0;
    usr1_fpu_ok = 0;
    usr1_sp = 0;
    usr2_sp = 0;

    fill_fpu(0x1234000000000000ULL);
    int64_t kr = sys_kill((int32_t)fastix_procinfo()->pid, FASTIX_SIGUSR1);
    int main_fpu_ok = check_fpu(0x1234000000000000ULL);

    ok &= kr == 0 && usr1_hits == 1 && usr2_hits == 1;
    ok &= usr1_sp >= alt_lo && usr1_sp < alt_hi;
    ok &= usr2_sp >= alt_lo && usr2_sp < usr1_sp; /* nested frame below usr1 frame */
    ok &= usr1_fpu_ok == 1 && main_fpu_ok == 1;

    /* Clean up handlers and altstack. */
    struct fastix_sigaction dfl = { .handler = FASTIX_SIG_DFL };
    struct fastix_sigaltstack dis = { .ss_flags = FASTIX_SS_DISABLE };
    ok &= sys_sigaction(FASTIX_SIGUSR1, &dfl, 0) == 0;
    ok &= sys_sigaction(FASTIX_SIGUSR2, &dfl, 0) == 0;
    ok &= fastix_sys2(FASTIX_SYS_sigaltstack, (uint64_t)(uintptr_t)&dis, 0) == 0;

    vmtest_report("signal_nested_altstack_and_fpu", ok);
}

/* ---- 2. sigprocmask, sigpending, sigsuspend, realtime FIFO queuing ---- */

static volatile int rt_log_n;
static volatile int rt_log_sig[16];
static volatile uint64_t rt_log_val[16];

static void handle_rt(int sig, struct fastix_siginfo *info, struct fastix_ucontext *uc)
{
    (void)uc;
    int idx = rt_log_n++;
    if (idx < 16) {
        rt_log_sig[idx] = sig;
        rt_log_val[idx] = info->si_value;
    }
}

static void t_mask_suspend_rt(void)
{
    int ok = 1;
    int rt1 = FASTIX_SIGRTMIN + 1;
    int rt2 = FASTIX_SIGRTMIN + 2;

    /* SIGKILL and SIGSTOP cannot be caught. */
    struct fastix_sigaction act = {
        .handler = (uint64_t)(uintptr_t)&handle_rt,
        .flags = FASTIX_SA_SIGINFO,
    };
    ok &= sys_sigaction(FASTIX_SIGKILL, &act, 0) == -FASTIX_EINVAL;
    ok &= sys_sigaction(FASTIX_SIGSTOP, &act, 0) == -FASTIX_EINVAL;

    ok &= sys_sigaction(FASTIX_SIGUSR1, &act, 0) == 0;
    ok &= sys_sigaction(rt1, &act, 0) == 0;
    ok &= sys_sigaction(rt2, &act, 0) == 0;

    /* Block SIGUSR1, rt1, rt2 (and attempt SIGKILL, which must be silently stripped). */
    uint64_t blk = fastix_sig_bit(FASTIX_SIGUSR1) | fastix_sig_bit(rt1) | fastix_sig_bit(rt2) |
                   fastix_sig_bit(FASTIX_SIGKILL);
    uint64_t old = 0;
    ok &= sys_sigprocmask(FASTIX_SIG_BLOCK, &blk, &old) == 0 && old == 0;

    rt_log_n = 0;
    uint32_t self = fastix_procinfo()->pid;
    /* Queue rt2 (100,101,102), rt1 (200,201), and SIGUSR1. */
    for (int i = 0; i < 3; i++) {
        struct fastix_siginfo si = { .si_code = FASTIX_SI_QUEUE, .si_value = (uint64_t)(100 + i) };
        ok &= fastix_sys3(FASTIX_SYS_rt_sigqueueinfo, self, (uint64_t)rt2,
                          (uint64_t)(uintptr_t)&si) == 0;
    }
    for (int i = 0; i < 2; i++) {
        struct fastix_siginfo si = { .si_code = FASTIX_SI_QUEUE, .si_value = (uint64_t)(200 + i) };
        ok &= fastix_sys3(FASTIX_SYS_rt_sigqueueinfo, self, (uint64_t)rt1,
                          (uint64_t)(uintptr_t)&si) == 0;
    }
    ok &= sys_kill((int32_t)self, FASTIX_SIGUSR1) == 0;

    /* Nothing delivered yet; all 3 must show up in sigpending. */
    uint64_t pend = 0;
    ok &= fastix_sys1(FASTIX_SYS_rt_sigpending, (uint64_t)(uintptr_t)&pend) == 0;
    ok &= rt_log_n == 0;
    ok &= (pend & fastix_sig_bit(FASTIX_SIGUSR1)) != 0;
    ok &= (pend & fastix_sig_bit(rt1)) != 0;
    ok &= (pend & fastix_sig_bit(rt2)) != 0;

    /* sigsuspend unblocking only SIGUSR1 must deliver SIGUSR1, return -EINTR, and restore mask. */
    uint64_t susp_mask = fastix_sig_bit(rt1) | fastix_sig_bit(rt2);
    ok &= fastix_sys1(FASTIX_SYS_rt_sigsuspend, (uint64_t)(uintptr_t)&susp_mask) == -FASTIX_EINTR;
    ok &= rt_log_n == 1 && rt_log_sig[0] == FASTIX_SIGUSR1;
    uint64_t after_susp = 0;
    ok &= sys_sigprocmask(FASTIX_SIG_BLOCK, 0, &after_susp) == 0;
    ok &= (after_susp & fastix_sig_bit(FASTIX_SIGUSR1)) != 0; /* original mask restored */
    ok &= (after_susp & fastix_sig_bit(FASTIX_SIGKILL)) == 0; /* SIGKILL never blocked */

    /* Unblock all: rt1 (200, 201) must arrive first, then rt2 (100, 101, 102) in FIFO order. */
    uint64_t zero = 0;
    ok &= sys_sigprocmask(FASTIX_SIG_SETMASK, &zero, 0) == 0;
    ok &= rt_log_n == 6;
    ok &= rt_log_sig[1] == rt1 && rt_log_val[1] == 200;
    ok &= rt_log_sig[2] == rt1 && rt_log_val[2] == 201;
    ok &= rt_log_sig[3] == rt2 && rt_log_val[3] == 100;
    ok &= rt_log_sig[4] == rt2 && rt_log_val[4] == 101;
    ok &= rt_log_sig[5] == rt2 && rt_log_val[5] == 102;

    struct fastix_sigaction dfl = { .handler = FASTIX_SIG_DFL };
    ok &= sys_sigaction(FASTIX_SIGUSR1, &dfl, 0) == 0;
    ok &= sys_sigaction(rt1, &dfl, 0) == 0;
    ok &= sys_sigaction(rt2, &dfl, 0) == 0;
    vmtest_report("signal_mask_pending_suspend_rt_queue", ok);
}

/* ---- 3. EINTR vs SA_RESTART (wait4 and nanosleep) ---- */

static volatile int eintr_hits;

static void handle_eintr(int sig)
{
    (void)sig;
    eintr_hits++;
}

static void t_eintr_restart(void)
{
    int ok = 1, st = 0;
    uint32_t parent_pid = fastix_procinfo()->pid;

    /* A. wait4 WITHOUT SA_RESTART -> returns -EINTR */
    struct fastix_sigaction sa = { .handler = (uint64_t)(uintptr_t)&handle_eintr, .flags = 0 };
    ok &= sys_sigaction(FASTIX_SIGUSR1, &sa, 0) == 0;
    eintr_hits = 0;

    int64_t c1 = fastix_sys0(FASTIX_SYS_fork);
    if (c1 == 0) {
        (void)sys_nanosleep_ms(15, 0);
        (void)sys_kill((int32_t)parent_pid, FASTIX_SIGUSR1);
        (void)sys_nanosleep_ms(15, 0);
        vmtest_exit(11);
    }
    int64_t wr = vmtest_wait4(c1, &st, 0);
    ok &= wr == -FASTIX_EINTR && eintr_hits == 1;
    ok &= vmtest_wait4(c1, &st, 0) == c1 && st == FASTIX_W_EXITED(11);

    /* B. wait4 WITH SA_RESTART -> restarts transparently and reaps child */
    sa.flags = FASTIX_SA_RESTART;
    ok &= sys_sigaction(FASTIX_SIGUSR1, &sa, 0) == 0;
    eintr_hits = 0;

    int64_t c2 = fastix_sys0(FASTIX_SYS_fork);
    if (c2 == 0) {
        (void)sys_nanosleep_ms(15, 0);
        (void)sys_kill((int32_t)parent_pid, FASTIX_SIGUSR1);
        (void)sys_nanosleep_ms(15, 0);
        vmtest_exit(44);
    }
    wr = vmtest_wait4(c2, &st, 0);
    ok &= wr == c2 && st == FASTIX_W_EXITED(44) && eintr_hits == 1;

    /* C. nanosleep WITH SA_RESTART -> NEVER restarted; returns -EINTR and writes rem */
    eintr_hits = 0;
    int64_t c3 = fastix_sys0(FASTIX_SYS_fork);
    if (c3 == 0) {
        (void)sys_nanosleep_ms(15, 0);
        (void)sys_kill((int32_t)parent_pid, FASTIX_SIGUSR1);
        vmtest_exit(0);
    }
    struct fastix_timespec rem = { 0, 0 };
    int64_t nr = sys_nanosleep_ms(200, &rem);
    uint64_t rem_ns = (uint64_t)rem.tv_sec * FASTIX_NSEC_PER_SEC + (uint64_t)rem.tv_nsec;
    ok &= nr == -FASTIX_EINTR && eintr_hits == 1 && rem_ns > 0 && rem_ns < 200000000ULL;
    ok &= vmtest_wait4(c3, &st, 0) == c3 && st == FASTIX_W_EXITED(0);

    struct fastix_sigaction dfl = { .handler = FASTIX_SIG_DFL };
    ok &= sys_sigaction(FASTIX_SIGUSR1, &dfl, 0) == 0;
    vmtest_report("signal_eintr_vs_sa_restart", ok);
}

/* ---- 4. SIGSTOP/SIGCONT, WUNTRACED/WCONTINUED, SIGCHLD & SA_NOCLDWAIT ---- */

static volatile int chld_hits, chld_last_code;

static void handle_chld(int sig, struct fastix_siginfo *info, struct fastix_ucontext *uc)
{
    (void)uc;
    if (sig == FASTIX_SIGCHLD) {
        chld_hits++;
        chld_last_code = info->si_code;
    }
}

static void t_stop_cont_sigchld(void)
{
    int ok = 1, st = 0;
    struct fastix_sigaction sa = {
        .handler = (uint64_t)(uintptr_t)&handle_chld,
        .flags = FASTIX_SA_SIGINFO | FASTIX_SA_RESTART,
    };
    ok &= sys_sigaction(FASTIX_SIGCHLD, &sa, 0) == 0;
    chld_hits = 0;
    chld_last_code = 0;

    int64_t c = fastix_sys0(FASTIX_SYS_fork);
    if (c == 0) {
        for (;;) {
            (void)fastix_sys0(FASTIX_SYS_sched_yield);
        }
    }
    ok &= c > 0;
    ok &= sys_kill((int32_t)c, FASTIX_SIGSTOP) == 0;
    ok &= vmtest_wait4(c, &st, FASTIX_WUNTRACED) == c && FASTIX_W_IFSTOPPED(st) &&
          FASTIX_W_STOPSIG(st) == FASTIX_SIGSTOP;
    ok &= chld_hits >= 1;

    ok &= sys_kill((int32_t)c, FASTIX_SIGCONT) == 0;
    ok &= vmtest_wait4(c, &st, FASTIX_WCONTINUED) == c && FASTIX_W_IFCONTINUED(st);

    ok &= sys_kill((int32_t)c, FASTIX_SIGTERM) == 0;
    ok &= vmtest_wait4(c, &st, 0) == c && st == FASTIX_W_SIGNALED(FASTIX_SIGTERM);

    /* SA_NOCLDWAIT auto-reaps exiting children (no zombie left behind). */
    struct fastix_sigaction noz = {
        .handler = FASTIX_SIG_DFL,
        .flags = FASTIX_SA_NOCLDWAIT,
    };
    ok &= sys_sigaction(FASTIX_SIGCHLD, &noz, 0) == 0;
    int64_t c2 = fastix_sys0(FASTIX_SYS_fork);
    if (c2 == 0) {
        vmtest_exit(0);
    }
    ok &= c2 > 0;
    /* Wait blocks until c2 exits, then returns -ECHILD because c2 was auto-reaped. */
    ok &= vmtest_wait4(-1, &st, 0) == -FASTIX_ECHILD;

    struct fastix_sigaction dfl = { .handler = FASTIX_SIG_DFL };
    ok &= sys_sigaction(FASTIX_SIGCHLD, &dfl, 0) == 0;
    vmtest_report("signal_stop_cont_sigchld_nocldwait", ok);
}

/* ---- 5. Timers: nanosleep, clock_nanosleep, setitimer/alarm, timer_create ---- */

static volatile int alrm_hits, ptimer_hits, ptimer_id_seen;
static volatile uint64_t ptimer_val_seen;

static void handle_alrm(int sig)
{
    if (sig == FASTIX_SIGALRM) {
        alrm_hits++;
    }
}

static void handle_ptimer(int sig, struct fastix_siginfo *info, struct fastix_ucontext *uc)
{
    (void)sig;
    (void)uc;
    if (info->si_code == FASTIX_SI_TIMER) {
        ptimer_hits++;
        ptimer_id_seen = info->si_timerid;
        ptimer_val_seen = info->si_value;
    }
}

static void t_timers(void)
{
    int ok = 1;

    /* A. nanosleep(25 ms) */
    uint64_t t0 = fastix_clock_mono_ns();
    ok &= sys_nanosleep_ms(25, 0) == 0;
    uint64_t dt = fastix_clock_mono_ns() - t0;
    ok &= dt >= 25000000ULL && dt < 2000000000ULL;

    /* B. clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, now + 20 ms) */
    uint64_t abs_target = fastix_clock_mono_ns() + 20000000ULL;
    struct fastix_timespec abs_ts = {
        .tv_sec = (int64_t)(abs_target / FASTIX_NSEC_PER_SEC),
        .tv_nsec = (int64_t)(abs_target % FASTIX_NSEC_PER_SEC),
    };
    ok &= fastix_sys4(FASTIX_SYS_clock_nanosleep, FASTIX_CLOCK_MONOTONIC, FASTIX_TIMER_ABSTIME,
                      (uint64_t)(uintptr_t)&abs_ts, 0) == 0;
    ok &= fastix_clock_mono_ns() >= abs_target;

    /* C. setitimer(ITIMER_REAL) / alarm */
    struct fastix_sigaction sa_alrm = { .handler = (uint64_t)(uintptr_t)&handle_alrm };
    ok &= sys_sigaction(FASTIX_SIGALRM, &sa_alrm, 0) == 0;
    alrm_hits = 0;

    struct fastix_itimerval itv = {
        .it_interval = { 0, 0 },
        .it_value = { 0, 15000 }, /* 15 ms */
    };
    uint64_t a0 = fastix_clock_mono_ns();
    ok &= fastix_sys3(FASTIX_SYS_setitimer, FASTIX_ITIMER_REAL, (uint64_t)(uintptr_t)&itv, 0) == 0;
    struct fastix_itimerval cur = { 0 };
    ok &= fastix_sys2(FASTIX_SYS_getitimer, FASTIX_ITIMER_REAL, (uint64_t)(uintptr_t)&cur) == 0;
    ok &= cur.it_value.tv_usec > 0 || alrm_hits > 0;
    for (int i = 0; i < 50 && alrm_hits == 0; i++) {
        (void)sys_nanosleep_ms(5, 0);
    }
    ok &= alrm_hits == 1 && (fastix_clock_mono_ns() - a0) >= 15000000ULL;

    /* D. POSIX timer_create / timer_settime (periodic) / timer_gettime / timer_delete */
    int ptsig = FASTIX_SIGRTMIN + 5;
    struct fastix_sigaction sa_pt = {
        .handler = (uint64_t)(uintptr_t)&handle_ptimer,
        .flags = FASTIX_SA_SIGINFO,
    };
    ok &= sys_sigaction(ptsig, &sa_pt, 0) == 0;
    ptimer_hits = 0;
    ptimer_id_seen = -1;
    ptimer_val_seen = 0;

    struct fastix_sigevent sev = {
        .sigev_value = 0xCAFEBABEULL,
        .sigev_signo = ptsig,
        .sigev_notify = FASTIX_SIGEV_SIGNAL,
    };
    int32_t tid = -1;
    ok &= fastix_sys3(FASTIX_SYS_timer_create, FASTIX_CLOCK_MONOTONIC, (uint64_t)(uintptr_t)&sev,
                      (uint64_t)(uintptr_t)&tid) == 0 && tid >= 0;

    struct fastix_itimerspec its = {
        .it_interval = { 0, 10000000 }, /* 10 ms periodic */
        .it_value = { 0, 10000000 },    /* 10 ms initial */
    };
    ok &= fastix_sys4(FASTIX_SYS_timer_settime, (uint64_t)(int64_t)tid, 0,
                      (uint64_t)(uintptr_t)&its, 0) == 0;
    struct fastix_itimerspec gt = { 0 };
    ok &= fastix_sys2(FASTIX_SYS_timer_gettime, (uint64_t)(int64_t)tid,
                      (uint64_t)(uintptr_t)&gt) == 0;
    ok &= gt.it_interval.tv_nsec == 10000000;

    for (int i = 0; i < 60 && ptimer_hits < 3; i++) {
        (void)sys_nanosleep_ms(5, 0);
    }
    ok &= ptimer_hits >= 3 && ptimer_id_seen == tid && ptimer_val_seen == 0xCAFEBABEULL;
    ok &= fastix_sys1(FASTIX_SYS_timer_delete, (uint64_t)(int64_t)tid) == 0;
    ok &= fastix_sys1(FASTIX_SYS_timer_delete, (uint64_t)(int64_t)tid) == -FASTIX_EINVAL;

    struct fastix_sigaction dfl = { .handler = FASTIX_SIG_DFL };
    ok &= sys_sigaction(FASTIX_SIGALRM, &dfl, 0) == 0;
    ok &= sys_sigaction(ptsig, &dfl, 0) == 0;
    vmtest_report("timers_nanosleep_itimer_posix_timer", ok);
}

void run_sig_tests(void)
{
    t_nested_altstack_fpu();
    t_mask_suspend_rt();
    t_eintr_restart();
    t_stop_cont_sigchld();
    t_timers();
}
