/*
 * test_wait.c - QEMU tests for waitid (exit/stop/continue reports, WNOWAIT, WNOHANG,
 * blocking wake-up) and rt_sigtimedwait (pending, poll, timeout, wake-up, EINTR).
 */
#include <stdint.h>
#include <fastix/abi.h>
#include <fastix/errno.h>
#include <fastix/signal_abi.h>
#include <fastix/time_abi.h>
#include <fastix_sys/sys.h>
#include "vmtest.h"

#define MS (1000000ULL)

static int64_t sys_sigaction(int sig, const struct fastix_sigaction *act)
{
    return fastix_sys3(FASTIX_SYS_rt_sigaction, (uint64_t)sig, (uint64_t)(uintptr_t)act, 0);
}

static int64_t sys_sigprocmask(int how, uint64_t set)
{
    return fastix_sys3(FASTIX_SYS_rt_sigprocmask, (uint64_t)how, (uint64_t)(uintptr_t)&set, 0);
}

static int64_t sys_kill(int64_t pid, int sig)
{
    return fastix_sys2(FASTIX_SYS_kill, (uint64_t)pid, (uint64_t)sig);
}

static void sleep_ms(uint64_t ms)
{
    struct fastix_timespec req = { 0, (int64_t)(ms * MS) };
    (void)fastix_sys2(FASTIX_SYS_nanosleep, (uint64_t)(uintptr_t)&req, 0);
}

static int64_t sys_waitid(uint64_t idtype, int64_t id, struct fastix_siginfo *si, uint64_t opts)
{
    return fastix_sys4(FASTIX_SYS_waitid, idtype, (uint64_t)id, (uint64_t)(uintptr_t)si, opts);
}

/* rt_sigtimedwait; timeout_ms < 0 means wait forever. */
static int64_t sys_sigtimedwait(uint64_t set, struct fastix_siginfo *si, int64_t timeout_ms)
{
    struct fastix_timespec ts = { timeout_ms / 1000, (timeout_ms % 1000) * (int64_t)MS };
    return fastix_sys4(FASTIX_SYS_rt_sigtimedwait, (uint64_t)(uintptr_t)&set,
                       (uint64_t)(uintptr_t)si, timeout_ms < 0 ? 0 : (uint64_t)(uintptr_t)&ts,
                       sizeof(uint64_t));
}

static int si_is(const struct fastix_siginfo *si, int64_t pid, int code, int status)
{
    return si->si_signo == FASTIX_SIGCHLD && si->si_pid == (uint32_t)pid && si->si_code == code &&
           si->si_status == status;
}

static _Noreturn void spin_forever(void)
{
    for (;;) {
        (void)fastix_sys0(FASTIX_SYS_sched_yield);
    }
}

static void t_waitid(void)
{
    struct vm_test_stats before = vmtest_get_stats();
    struct fastix_siginfo si;
    int ok = 1, st = 0;

    /* A. WNOWAIT reports an exit but leaves the child waitable; the second call reaps it. */
    int64_t c = fastix_sys0(FASTIX_SYS_fork);
    if (c == 0) {
        vmtest_exit(7);
    }
    ok &= sys_waitid(FASTIX_P_PID, c, &si, FASTIX_WEXITED | FASTIX_WNOWAIT) == 0;
    ok &= si_is(&si, c, FASTIX_CLD_EXITED, 7);
    ok &= sys_waitid(FASTIX_P_PID, c, &si, FASTIX_WEXITED) == 0;
    ok &= si_is(&si, c, FASTIX_CLD_EXITED, 7);
    ok &= vmtest_wait4(c, &st, 0) == -FASTIX_ECHILD;

    /* B. WNOHANG with a running child gives si_pid 0; a blocking waitid is woken by the exit. */
    c = fastix_sys0(FASTIX_SYS_fork);
    if (c == 0) {
        sleep_ms(30);
        vmtest_exit(3);
    }
    si.si_pid = 12345;
    ok &= sys_waitid(FASTIX_P_ALL, 0, &si, FASTIX_WEXITED | FASTIX_WNOHANG) == 0 && si.si_pid == 0;
    ok &= sys_waitid(FASTIX_P_ALL, 0, &si, FASTIX_WEXITED) == 0;
    ok &= si_is(&si, c, FASTIX_CLD_EXITED, 3);

    /* C. Stop, continue and kill are reported with the right si_code and si_status. */
    c = fastix_sys0(FASTIX_SYS_fork);
    if (c == 0) {
        spin_forever();
    }
    ok &= sys_kill(c, FASTIX_SIGSTOP) == 0;
    ok &= sys_waitid(FASTIX_P_PID, c, &si, FASTIX_WSTOPPED) == 0;
    ok &= si_is(&si, c, FASTIX_CLD_STOPPED, FASTIX_SIGSTOP);
    ok &= sys_kill(c, FASTIX_SIGCONT) == 0;
    ok &= sys_waitid(FASTIX_P_PID, c, &si, FASTIX_WCONTINUED) == 0;
    ok &= si_is(&si, c, FASTIX_CLD_CONTINUED, FASTIX_SIGCONT);
    ok &= sys_kill(c, FASTIX_SIGTERM) == 0;
    ok &= sys_waitid(FASTIX_P_PGID, 0, &si, FASTIX_WEXITED) == 0; /* own process group */
    ok &= si_is(&si, c, FASTIX_CLD_KILLED, FASTIX_SIGTERM);

    /* D. Bad arguments and no children. */
    ok &= sys_waitid(FASTIX_P_ALL, 0, &si, FASTIX_WNOHANG) == -FASTIX_EINVAL; /* no kind */
    ok &= sys_waitid(9, 0, &si, FASTIX_WEXITED) == -FASTIX_EINVAL;
    ok &= sys_waitid(FASTIX_P_PID, 0, &si, FASTIX_WEXITED) == -FASTIX_EINVAL;
    ok &= sys_waitid(FASTIX_P_ALL, 0, &si, FASTIX_WEXITED) == -FASTIX_ECHILD;
    ok &= vmtest_wait4(-1, &st, FASTIX_WEXITED) == -FASTIX_EINVAL; /* wait4 keeps its options */

    ok &= vmtest_same_stats(before, vmtest_get_stats());
    vmtest_report("waitid_exit_stop_cont_nowait", ok);
}

static volatile int usr2_hits;

static void handle_usr2(int sig)
{
    if (sig == FASTIX_SIGUSR2) {
        usr2_hits++;
    }
}

static void t_sigtimedwait(void)
{
    struct vm_test_stats before = vmtest_get_stats();
    struct fastix_siginfo si;
    int ok = 1, st = 0;
    int rt1 = FASTIX_SIGRTMIN + 1;
    uint64_t waited = fastix_sig_bit(FASTIX_SIGUSR1) | fastix_sig_bit(rt1);
    uint32_t self = fastix_procinfo()->pid;

    ok &= sys_sigprocmask(FASTIX_SIG_BLOCK, waited) == 0;

    /* A. An already pending signal is returned at once, with its siginfo. */
    struct fastix_siginfo q = { .si_code = FASTIX_SI_QUEUE, .si_value = 55 };
    ok &= fastix_sys3(FASTIX_SYS_rt_sigqueueinfo, self, (uint64_t)rt1, (uint64_t)(uintptr_t)&q) == 0;
    ok &= sys_sigtimedwait(waited, &si, -1) == rt1;
    ok &= si.si_signo == rt1 && si.si_code == FASTIX_SI_QUEUE && si.si_value == 55;

    /* B. Zero timeout polls; a short timeout expires with EAGAIN after the full time. */
    ok &= sys_sigtimedwait(waited, &si, 0) == -FASTIX_EAGAIN;
    uint64_t t0 = fastix_clock_mono_ns();
    ok &= sys_sigtimedwait(waited, &si, 20) == -FASTIX_EAGAIN;
    uint64_t dt = fastix_clock_mono_ns() - t0;
    ok &= dt >= 20 * MS && dt < 2000 * MS;

    /* C. A signal from another process wakes the waiter before its timeout; the cancelled
     * timeout must not fire later (procd would reply to a reply cap it no longer holds). */
    int64_t c = fastix_sys0(FASTIX_SYS_fork);
    if (c == 0) {
        sleep_ms(15);
        (void)sys_kill(self, FASTIX_SIGUSR1);
        vmtest_exit(0);
    }
    ok &= sys_sigtimedwait(waited, &si, 100) == FASTIX_SIGUSR1;
    ok &= si.si_signo == FASTIX_SIGUSR1 && si.si_pid == (uint32_t)c && si.si_code == FASTIX_SI_USER;
    sleep_ms(150);
    ok &= vmtest_wait4(c, &st, 0) == c && st == FASTIX_W_EXITED(0);

    /* D. An unblocked signal that is not waited for runs its handler and gives EINTR. */
    struct fastix_sigaction act = { .handler = (uint64_t)(uintptr_t)&handle_usr2 };
    ok &= sys_sigaction(FASTIX_SIGUSR2, &act) == 0;
    usr2_hits = 0;
    c = fastix_sys0(FASTIX_SYS_fork);
    if (c == 0) {
        sleep_ms(15);
        (void)sys_kill(self, FASTIX_SIGUSR2);
        vmtest_exit(0);
    }
    ok &= sys_sigtimedwait(waited, &si, 100) == -FASTIX_EINTR && usr2_hits == 1;
    sleep_ms(150);
    ok &= vmtest_wait4(c, &st, 0) == c;

    /* E. Bad arguments. */
    ok &= fastix_sys4(FASTIX_SYS_rt_sigtimedwait, (uint64_t)(uintptr_t)&waited, 0, 0, 4) ==
          -FASTIX_EINVAL;
    struct fastix_timespec bad = { 0, 1000000000 };
    ok &= fastix_sys4(FASTIX_SYS_rt_sigtimedwait, (uint64_t)(uintptr_t)&waited, 0,
                      (uint64_t)(uintptr_t)&bad, sizeof(uint64_t)) == -FASTIX_EINVAL;

    struct fastix_sigaction dfl = { .handler = FASTIX_SIG_DFL };
    ok &= sys_sigaction(FASTIX_SIGUSR2, &dfl) == 0;
    ok &= sys_sigprocmask(FASTIX_SIG_UNBLOCK, waited) == 0;
    ok &= vmtest_same_stats(before, vmtest_get_stats());
    vmtest_report("sigtimedwait_pending_timeout_wake_eintr", ok);
}

void run_wait_tests(void)
{
    t_waitid();
    t_sigtimedwait();
}
