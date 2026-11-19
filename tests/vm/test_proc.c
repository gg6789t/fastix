/*
 * test_proc.c - Phase 3A behavioral tests: execve (ELF + #! scripts), vfork,
 * posix_spawn_fast, process groups & sessions, credentials, MAP_SHARED across
 * fork, orphan reparenting to PID 1, and 1,000x fork+exit+waitpid zero-leak.
 */
#include <stdint.h>

#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <fastix/mman.h>
#include <fastix/signal_abi.h>
#include <fastix_sys/sys.h>

#include "vmtest.h"

#define PAGE FASTIX_PAGE_SIZE
#define RW   (FASTIX_PROT_READ | FASTIX_PROT_WRITE)

static int64_t sys_execve(const char *path, const char *const *argv, const char *const *envp)
{
    return fastix_sys3(FASTIX_SYS_execve, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)argv,
                       (uint64_t)(uintptr_t)envp);
}

#define BIG_ENV_COUNT 1400
#define BIG_ENV_LEN   100 /* 99 x 'E' + NUL */
static char big_env[BIG_ENV_COUNT][BIG_ENV_LEN];
static const char *big_envp[BIG_ENV_COUNT + 1];

/* ~100 KB of environment must pass execve (FASTIX_ARG_MAX is 128 KB); ~140 KB must not. */
static int exec_big_env(void)
{
    const char *argv[] = { "/bin/exec_helper", "check_big", 0 };
    int st = 0;

    for (int i = 0; i < BIG_ENV_COUNT; i++) {
        for (int k = 0; k < BIG_ENV_LEN - 1; k++) {
            big_env[i][k] = 'E';
        }
        big_env[i][BIG_ENV_LEN - 1] = '\0';
        big_envp[i] = big_env[i];
    }
    big_envp[1000] = 0;
    int64_t pid = fastix_sys0(FASTIX_SYS_fork);
    if (pid == 0) {
        (void)sys_execve("/bin/exec_helper", argv, big_envp);
        vmtest_exit(2);
    }
    int ok = pid > 0 && vmtest_wait4(pid, &st, 0) == pid && st == FASTIX_W_EXITED(93);
    big_envp[1000] = big_env[1000];
    big_envp[BIG_ENV_COUNT] = 0;
    return ok && sys_execve("/bin/exec_helper", argv, big_envp) == -FASTIX_E2BIG;
}

static void t_execve(void)
{
    int ok = 1, st = 0;

    /* 1. ELF execve with argv, envp, and credentials preserved across exec. */
    int64_t pid = fastix_sys0(FASTIX_SYS_fork);
    if (pid == 0) {
        if (fastix_sys3(FASTIX_SYS_setresgid, 2000, 2001, 2002) != 0 ||
            fastix_sys3(FASTIX_SYS_setresuid, 1000, 1001, 1002) != 0) {
            vmtest_exit(1);
        }
        const char *argv[] = { "/bin/exec_helper", "check_exec", "arg2", 0 };
        const char *envp[] = { "FOO=bar", 0 };
        (void)sys_execve("/bin/exec_helper", argv, envp);
        vmtest_exit(2);
    }
    ok &= pid > 0 && vmtest_wait4(pid, &st, 0) == pid && st == FASTIX_W_EXITED(77);

    /* 2. #! basic script with optional argument. */
    pid = fastix_sys0(FASTIX_SYS_fork);
    if (pid == 0) {
        const char *argv[] = { "ignored0", "userarg", 0 };
        const char *envp[] = { 0 };
        (void)sys_execve("/bin/sh_basic", argv, envp);
        vmtest_exit(3);
    }
    ok &= pid > 0 && vmtest_wait4(pid, &st, 0) == pid && st == FASTIX_W_EXITED(88);

    /* 3. #! 4-level chain (depth == 4 succeeds). */
    pid = fastix_sys0(FASTIX_SYS_fork);
    if (pid == 0) {
        const char *argv[] = { "ignored0", "end", 0 };
        const char *envp[] = { 0 };
        (void)sys_execve("/bin/sh_chain1", argv, envp);
        vmtest_exit(4);
    }
    ok &= pid > 0 && vmtest_wait4(pid, &st, 0) == pid && st == FASTIX_W_EXITED(89);

    /* 4. Error cases must fail without destroying caller. */
    const char *empty_v[] = { "x", 0 };
    ok &= sys_execve("/bin/sh_too_deep", empty_v, empty_v) == -FASTIX_ELOOP;
    ok &= sys_execve("/bin/sh_loop", empty_v, empty_v) == -FASTIX_ELOOP;
    ok &= sys_execve("/bin/sh_bad", empty_v, empty_v) == -FASTIX_ENOEXEC;
    ok &= sys_execve("/bin/not_elf", empty_v, empty_v) == -FASTIX_ENOEXEC;
    ok &= sys_execve("/bin/no_such_bin", empty_v, empty_v) == -FASTIX_ENOENT;
    ok &= fastix_sys3(FASTIX_SYS_execve, 8, 0, 0) == -FASTIX_EFAULT;
    ok &= exec_big_env();

    vmtest_report("execve_elf_and_shebang_depth4", ok);
}

static volatile int vfork_shared_val;

static int run_vfork_once(void)
{
    int ok = 1, st = 0;

    /* 1. vfork + shared write + _exit */
    vfork_shared_val = 10;
    int64_t pid = fastix_sys0(FASTIX_SYS_vfork);
    if (pid == 0) {
        vfork_shared_val = 42;
        vmtest_exit(33);
    }
    /* Parent MUST see 42 immediately because parent stayed blocked until child exited. */
    ok &= pid > 0 && vfork_shared_val == 42;
    ok &= vmtest_wait4(pid, &st, 0) == pid && st == FASTIX_W_EXITED(33);

    /* 2. vfork + execve */
    vfork_shared_val = 1;
    pid = fastix_sys0(FASTIX_SYS_vfork);
    if (pid == 0) {
        vfork_shared_val = 99;
        const char *argv[] = { "/bin/exec_helper", "check_vfork", 0 };
        const char *envp[] = { 0 };
        (void)sys_execve("/bin/exec_helper", argv, envp);
        vmtest_exit(5);
    }
    ok &= pid > 0 && vfork_shared_val == 99;
    ok &= vmtest_wait4(pid, &st, 0) == pid && st == FASTIX_W_EXITED(91);

    /* 3. Nested vfork (parent -> child -> grandchild) */
    vfork_shared_val = 0;
    pid = fastix_sys0(FASTIX_SYS_vfork);
    if (pid == 0) {
        int64_t gc = fastix_sys0(FASTIX_SYS_vfork);
        if (gc == 0) {
            vfork_shared_val = 777;
            vmtest_exit(12);
        }
        int gc_st = 0;
        int gc_ok = gc > 0 && vfork_shared_val == 777 && vmtest_wait4(gc, &gc_st, 0) == gc &&
                    gc_st == FASTIX_W_EXITED(12);
        vmtest_exit(gc_ok ? 0 : 1);
    }
    ok &= pid > 0 && vfork_shared_val == 777;
    ok &= vmtest_wait4(pid, &st, 0) == pid && st == FASTIX_W_EXITED(0);
    return ok;
}

static void t_vfork(void)
{
    /* Warm up pools (including 2-deep nested reply objects), then check 5x zero leak. */
    int ok = run_vfork_once();
    struct vm_test_stats before = vmtest_get_stats();
    for (int i = 0; i < 5 && ok; i++) {
        ok &= run_vfork_once();
    }
    ok &= vmtest_same_stats(before, vmtest_get_stats());
    vmtest_report("vfork_shared_exec_and_nested", ok);
}

static void t_posix_spawn(void)
{
    int ok = 1, st = 0;
    const char *argv[] = { "/bin/exec_helper", "check_spawn", "self", 0 };
    const char *envp[] = { "SPAWN=ok", 0 };
    struct fastix_spawn_args sa = {
        .path = (uint64_t)(uintptr_t)"/bin/exec_helper",
        .argv = (uint64_t)(uintptr_t)argv,
        .envp = (uint64_t)(uintptr_t)envp,
        .flags = FASTIX_SPAWN_SETPGROUP,
        .pgroup = 0, /* child becomes its own process group */
    };

    /* Warm up, then check zero leaks on both success and failure. */
    int64_t p0 = fastix_sys1(FASTIX_SYS_posix_spawn_fast, (uint64_t)(uintptr_t)&sa);
    ok &= p0 > 0 && vmtest_wait4(p0, &st, 0) == p0 && st == FASTIX_W_EXITED(90);
    struct vm_test_stats before = vmtest_get_stats();

    int64_t p1 = fastix_sys1(FASTIX_SYS_posix_spawn_fast, (uint64_t)(uintptr_t)&sa);
    ok &= p1 > 0 && vmtest_wait4(p1, &st, 0) == p1 && st == FASTIX_W_EXITED(90);

    /* Failure cases must leak nothing. */
    struct fastix_spawn_args bad = sa;
    bad.path = (uint64_t)(uintptr_t)"/bin/no_such_bin";
    ok &= fastix_sys1(FASTIX_SYS_posix_spawn_fast, (uint64_t)(uintptr_t)&bad) == -FASTIX_ENOENT;
    bad.path = (uint64_t)(uintptr_t)"/bin/sh_loop";
    ok &= fastix_sys1(FASTIX_SYS_posix_spawn_fast, (uint64_t)(uintptr_t)&bad) == -FASTIX_ELOOP;

    ok &= vmtest_same_stats(before, vmtest_get_stats());
    vmtest_report("posix_spawn_fast_pgroup_and_errors", ok);
}

static void t_pgid_sid(void)
{
    int ok = 1, st = 0;

    /* 1. setsid in a non-leader child updates procinfo and rejects a second setsid. */
    int64_t c1 = fastix_sys0(FASTIX_SYS_fork);
    if (c1 == 0) {
        const struct fastix_procinfo *info = fastix_procinfo();
        int64_t mypid = info->pid;
        int64_t sid = fastix_sys0(FASTIX_SYS_setsid);
        int cok = sid == mypid && info->sid == (uint32_t)mypid && info->pgid == (uint32_t)mypid &&
                  fastix_sys1(FASTIX_SYS_getsid, 0) == mypid &&
                  fastix_sys1(FASTIX_SYS_getpgid, 0) == mypid &&
                  fastix_sys0(FASTIX_SYS_setsid) == -FASTIX_EPERM;
        vmtest_exit(cok ? 0 : 1);
    }
    ok &= c1 > 0 && vmtest_wait4(c1, &st, 0) == c1 && st == FASTIX_W_EXITED(0);

    /* 2. Two children in different process groups; wait4(-pgid) reaps only the matching one. */
    int64_t a = fastix_sys0(FASTIX_SYS_fork);
    if (a == 0) {
        if (fastix_sys2(FASTIX_SYS_setpgid, 0, 0) != 0) {
            vmtest_exit(1);
        }
        if (fastix_procinfo()->pgid != fastix_procinfo()->pid) {
            vmtest_exit(2);
        }
        vmtest_exit(11);
    }
    int64_t b = fastix_sys0(FASTIX_SYS_fork);
    if (b == 0) {
        vmtest_exit(22); /* stays in parent's pgid */
    }
    /* Ensure 'a' is in its own pgid (parent calling setpgid is idempotent/race-free). */
    (void)fastix_sys2(FASTIX_SYS_setpgid, (uint64_t)a, (uint64_t)a);
    ok &= vmtest_wait4(-a, &st, 0) == a && st == FASTIX_W_EXITED(11);
    ok &= vmtest_wait4(0, &st, 0) == b && st == FASTIX_W_EXITED(22);

    /* 3. setpgid on a child after exec returns EACCES. */
    const char *argv[] = { "/bin/exec_helper", "check_vfork", 0 };
    const char *envp[] = { 0 };
    struct fastix_spawn_args sa = {
        .path = (uint64_t)(uintptr_t)"/bin/exec_helper",
        .argv = (uint64_t)(uintptr_t)argv,
        .envp = (uint64_t)(uintptr_t)envp,
    };
    int64_t c2 = fastix_sys1(FASTIX_SYS_posix_spawn_fast, (uint64_t)(uintptr_t)&sa);
    int64_t spg = fastix_sys2(FASTIX_SYS_setpgid, (uint64_t)c2, (uint64_t)c2);
    /* Child might already have exited (ESRCH) or still be alive after exec (EACCES). */
    ok &= c2 > 0 && (spg == -FASTIX_EACCES || spg == -FASTIX_ESRCH);
    ok &= vmtest_wait4(c2, &st, 0) == c2 && st == FASTIX_W_EXITED(91);

    vmtest_report("pgid_sid_and_wait_pgroup", ok);
}

static void t_creds(void)
{
    int ok = 1, st = 0;

    int64_t pid = fastix_sys0(FASTIX_SYS_fork);
    if (pid == 0) {
        const struct fastix_procinfo *info = fastix_procinfo();
        int cok = info->uid == 0 && info->euid == 0 && info->gid == 0 && info->egid == 0;

        uint32_t old_um = (uint32_t)fastix_sys1(FASTIX_SYS_umask, 0077);
        cok &= old_um == 022 && info->umask == 0077;

        cok &= fastix_sys3(FASTIX_SYS_setresgid, 500, 501, 502) == 0;
        cok &= info->gid == 500 && info->egid == 501;
        uint32_t r = 0, e = 0, s = 0;
        cok &= fastix_sys3(FASTIX_SYS_getresgid, (uint64_t)(uintptr_t)&r, (uint64_t)(uintptr_t)&e,
                           (uint64_t)(uintptr_t)&s) == 0;
        cok &= r == 500 && e == 501 && s == 502;

        cok &= fastix_sys3(FASTIX_SYS_setresuid, 100, 101, 102) == 0;
        cok &= info->uid == 100 && info->euid == 101;
        cok &= fastix_sys3(FASTIX_SYS_getresuid, (uint64_t)(uintptr_t)&r, (uint64_t)(uintptr_t)&e,
                           (uint64_t)(uintptr_t)&s) == 0;
        cok &= r == 100 && e == 101 && s == 102;

        /* Unprivileged: can switch euid to saved uid 102, cannot become root (0). */
        cok &= fastix_sys1(FASTIX_SYS_seteuid, 102) == 0 && info->euid == 102;
        cok &= fastix_sys1(FASTIX_SYS_setuid, 0) == -FASTIX_EPERM;
        cok &= fastix_sys1(FASTIX_SYS_setgid, 0) == -FASTIX_EPERM;
        vmtest_exit(cok ? 0 : 1);
    }
    ok &= pid > 0 && vmtest_wait4(pid, &st, 0) == pid && st == FASTIX_W_EXITED(0);
    vmtest_report("credentials_and_procinfo_sync", ok);
}

static int run_map_shared_once(void)
{
    int ok = 1, st = 0;
    int64_t a = fastix_sys6(FASTIX_SYS_mmap, 0, 2 * PAGE, RW,
                            FASTIX_MAP_SHARED | FASTIX_MAP_ANONYMOUS, (uint64_t)-1, 0);
    ok &= a > 0;
    volatile uint64_t *p0 = (volatile uint64_t *)(uintptr_t)a;
    volatile uint64_t *p1 = (volatile uint64_t *)(uintptr_t)((uint64_t)a + PAGE);

    /* Touch page 0 before fork; leave page 1 unfaulted (lazy) before fork! */
    *p0 = 0x11111111ULL;

    int64_t pid = fastix_sys0(FASTIX_SYS_fork);
    if (pid == 0) {
        int cok = (*p0 == 0x11111111ULL && *p1 == 0);
        *p0 = 0x22222222ULL;
        *p1 = 0x33333333ULL;
        vmtest_exit(cok ? 0 : 1);
    }
    ok &= pid > 0 && vmtest_wait4(pid, &st, 0) == pid && st == FASTIX_W_EXITED(0);
    ok &= *p0 == 0x22222222ULL && *p1 == 0x33333333ULL;

    /* Partial munmap keeps page 1 intact; final munmap leaks 0 frames. */
    ok &= fastix_sys2(FASTIX_SYS_munmap, (uint64_t)a, PAGE) == 0;
    ok &= *p1 == 0x33333333ULL;
    ok &= fastix_sys2(FASTIX_SYS_munmap, (uint64_t)a + PAGE, PAGE) == 0;
    return ok;
}

static void t_map_shared(void)
{
    /* Warm up shm slab caches once, then verify 5 cycles leak 0 frames/bytes/slots. */
    int ok = run_map_shared_once();
    struct vm_test_stats before = vmtest_get_stats();
    for (int i = 0; i < 5 && ok; i++) {
        ok &= run_map_shared_once();
    }
    ok &= vmtest_same_stats(before, vmtest_get_stats());
    vmtest_report("map_shared_anon_across_fork", ok);
}

static void t_orphan_reparent(void)
{
    int ok = 1, st = 0;

    /* Shared page so grandchild can report its observed ppid after parent dies. */
    int64_t shm = fastix_sys6(FASTIX_SYS_mmap, 0, PAGE, RW,
                              FASTIX_MAP_SHARED | FASTIX_MAP_ANONYMOUS, (uint64_t)-1, 0);
    ok &= shm > 0;
    volatile uint32_t *box = (volatile uint32_t *)(uintptr_t)shm;
    box[0] = 0; /* grandchild pid */
    box[1] = 0; /* observed ppid via procinfo after reparent */
    box[2] = 0; /* observed ppid via getppid after reparent */

    int64_t mid = fastix_sys0(FASTIX_SYS_fork);
    if (mid == 0) {
        int64_t gc = fastix_sys0(FASTIX_SYS_fork);
        if (gc == 0) {
            box[0] = fastix_procinfo()->pid;
            /* Yield until our parent exits and procd reparents us to PID 1. */
            for (int i = 0; i < 10000 && fastix_procinfo()->ppid != 1; i++) {
                (void)fastix_sys0(FASTIX_SYS_sched_yield);
            }
            box[1] = fastix_procinfo()->ppid;
            box[2] = (uint32_t)fastix_sys0(FASTIX_SYS_getppid);
            vmtest_exit(55);
        }
        /* Wait briefly so gc has started, then exit immediately to orphan gc. */
        vmtest_exit(0);
    }

    ok &= mid > 0 && vmtest_wait4(mid, &st, 0) == mid && st == FASTIX_W_EXITED(0);
    /* Now PID 1 (us!) must reap the reparented grandchild. */
    int64_t reaped = vmtest_wait4(-1, &st, 0);
    ok &= reaped > 0 && (uint32_t)reaped == box[0] && st == FASTIX_W_EXITED(55);
    ok &= box[1] == 1 && box[2] == 1;
    ok &= fastix_sys2(FASTIX_SYS_munmap, (uint64_t)shm, PAGE) == 0;
    vmtest_report("orphan_reparent_to_pid1", ok);
}

static void t_fork_1000(void)
{
    int ok = 1, st = 0;

    /* Warm up once, then run 1,000 fork+exit+waitpid cycles and require 0 leaks. */
    int64_t w = fastix_sys0(FASTIX_SYS_fork);
    if (w == 0) {
        vmtest_exit(0);
    }
    ok &= w > 0 && vmtest_wait4(w, &st, 0) == w;
    struct vm_test_stats before = vmtest_get_stats();

    for (int i = 0; i < 1000 && ok; i++) {
        int64_t p = fastix_sys0(FASTIX_SYS_fork);
        if (p == 0) {
            vmtest_exit(i & 0x7f);
        }
        int status = -1;
        ok &= p > 0 && vmtest_wait4(p, &status, 0) == p && status == FASTIX_W_EXITED(i & 0x7f);
    }
    ok &= vmtest_same_stats(before, vmtest_get_stats());
    vmtest_report("fork_exit_wait_1000_zero_leak", ok);
}

void run_proc_tests(void)
{
    t_execve();
    t_vfork();
    t_posix_spawn();
    t_pgid_sid();
    t_creds();
    t_map_shared();
    t_orphan_reparent();
    t_fork_1000();
}
