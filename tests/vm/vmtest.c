/*
 * vmtest.c - Phase 2 behavioural tests, run as PID 1. Each test prints
 * [FASTIX-TEST PASS|FAIL] name; the program exits with the failure count,
 * and procd turns PID 1's exit into [FASTIX-SUITE DONE rc=N].
 * Faults that must kill a process are tried in a forked child, and the
 * parent checks how the child died.
 */
#include <stdint.h>

#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <fastix/mman.h>
#include <fastix/signal_abi.h>
#include <fastix/vspace.h>
#include <fastix_sys/sys.h>
#include <libos/kprint.h>

#include "vmtest.h"

#define PAGE           FASTIX_PAGE_SIZE
#define RW             (FASTIX_PROT_READ | FASTIX_PROT_WRITE)
#define ANON           (FASTIX_MAP_PRIVATE | FASTIX_MAP_ANONYMOUS)
#define IMAGE_STATIC   1
#define IMAGE_PIE      2
#define HELLO_OK       42
#define NULL_ADDR      8

_Noreturn void prog_main(uint64_t *sp);

static int failures;

/* ---- syscall wrappers ---- */

static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags)
{
    return fastix_sys6(FASTIX_SYS_mmap, addr, len, prot, flags, (uint64_t)-1, 0);
}

static int64_t sys_munmap(uint64_t addr, uint64_t len)
{
    return fastix_sys2(FASTIX_SYS_munmap, addr, len);
}

static int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot)
{
    return fastix_sys3(FASTIX_SYS_mprotect, addr, len, prot);
}

static _Noreturn void sys_exit(int code)
{
    (void)fastix_sys1(FASTIX_SYS_exit_group, (uint64_t)code);
    for (;;) {
    }
}

_Noreturn void vmtest_exit(int code)
{
    sys_exit(code);
}

/* wait4 for `pid`; returns the pid (or -errno) and the status word in *status. */
int64_t vmtest_wait4(int64_t pid, int *status, uint64_t options)
{
    uint64_t args[3] = { (uint64_t)pid, 0, options }, more[FASTIX_CALL_MORE];
    int64_t r = fastix_call(FASTIX_SYS_wait4, 3, args, more);
    if (status != 0) {
        *status = (int)more[0];
    }
    return r;
}

static int64_t sys_wait(int64_t pid, int *status)
{
    return vmtest_wait4(pid, status, 0);
}

struct vm_test_stats vmtest_get_stats(void)
{
    struct vm_test_stats s;
    (void)fastix_call(FASTIX_TEST_STATS, 0, 0, s.v);
    return s;
}

int vmtest_same_stats(struct vm_test_stats a, struct vm_test_stats b)
{
    for (int i = 0; i < 6; i++) {
        if (a.v[i] != b.v[i]) {
            kprint_str("  stats differ at index ");
            kprint_u64((uint64_t)i);
            kprint_str(": ");
            kprint_u64(a.v[i]);
            kprint_str(" -> ");
            kprint_u64(b.v[i]);
            kprint_str("\n");
            return 0;
        }
    }
    return 1;
}

void vmtest_report(const char *name, int ok)
{
    kprint_str(ok ? "[FASTIX-TEST PASS] " : "[FASTIX-TEST FAIL] ");
    kprint_str(name);
    kprint_str("\n");
    failures += !ok;
}

#define stats      vm_test_stats
#define get_stats  vmtest_get_stats
#define same_stats vmtest_same_stats
#define report     vmtest_report

/* ---- running something in a child ---- */

static int test_arg; /* argument for child functions (copied by fork) */

/* Forks, runs fn() in the child (which then exits 0), and returns the child's wait status. */
static int in_child(void (*fn)(void), int64_t *pid_out)
{
    int64_t pid = fastix_sys0(FASTIX_SYS_fork);
    if (pid == 0) {
        fn();
        sys_exit(0);
    }
    int status = -1;
    if (pid < 0 || sys_wait(pid, &status) != pid) {
        return -1;
    }
    if (pid_out != 0) {
        *pid_out = pid;
    }
    return status;
}

/* True if child `pid` was killed by SIGSEGV with `code`, at `addr` (any addr if 0). */
static int killed_by_segv(int status, int64_t pid, int code, uint64_t addr)
{
    uint64_t args[1] = { (uint64_t)pid }, more[FASTIX_CALL_MORE];
    if (status != FASTIX_W_SIGNALED(FASTIX_SIGSEGV) ||
        fastix_call(FASTIX_TEST_LAST_KILL, 1, args, more) != 0) {
        return 0;
    }
    return more[0] == FASTIX_SIGSEGV && (int)more[1] == code && (addr == 0 || more[2] == addr);
}

static uint64_t touched_addr; /* address child functions touch */

static void child_read(void)
{
    (void)*(volatile const uint8_t *)(uintptr_t)touched_addr;
}

static void child_write(void)
{
    *(volatile uint8_t *)(uintptr_t)touched_addr = 1;
}

/* ---- tests ---- */

static void t_ipc_100k(void)
{
    uint64_t wrong = 0;
    for (uint64_t i = 0; i < 100000; i++) {
        wrong += fastix_sys1(FASTIX_TEST_PING, i) != (int64_t)(i + 1);
    }
    report("ipc_100k_calls", wrong == 0);
}

static void t_vdso(void)
{
    uint64_t more[FASTIX_CALL_MORE], args[1] = { FASTIX_CLOCK_MONOTONIC };
    const struct fastix_procinfo *info = fastix_procinfo();
    int ok = info->pid == 1 && fastix_sys0(FASTIX_SYS_getpid) == 1 && info->ppid == 0;

    uint64_t t1 = fastix_clock_mono_ns();
    ok &= fastix_call(FASTIX_SYS_clock_gettime, 1, args, more) == 0;
    uint64_t t3 = fastix_clock_mono_ns();
    ok &= t1 <= more[0] && more[0] <= t3 && t3 - t1 < 1000000000ULL;

    /* 10,000 clock + pid reads must not send a single message to procd. */
    (void)fastix_call(FASTIX_TEST_IPC_COUNT, 0, 0, more);
    uint64_t before = more[0], last = 0, pid_sum = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t now = fastix_clock_mono_ns();
        ok &= now >= last;
        last = now;
        pid_sum += fastix_procinfo()->pid;
    }
    (void)fastix_call(FASTIX_TEST_IPC_COUNT, 0, 0, more);
    ok &= more[0] == before + 1 && pid_sum == 10000;
    report("vdso_getpid_clock_zero_ipc", ok);
}

static int mmap_touch_unmap(int i)
{
    int64_t a = sys_mmap(0, 16 * PAGE, RW, ANON);
    int ok = a > 0;
    for (uint64_t p = 0; ok && p < 16; p++) {
        volatile uint64_t *w = (volatile uint64_t *)(uintptr_t)((uint64_t)a + p * PAGE);
        ok &= *w == 0; /* fresh memory is zero */
        *w = (uint64_t)i * 100 + p;
        ok &= *w == (uint64_t)i * 100 + p;
    }
    return ok && sys_munmap((uint64_t)a, 16 * PAGE) == 0;
}

static void t_mmap_500(void)
{
    /* Warm-up: the first mapping in the mmap region creates page tables that stay
     * with the address space, and procd's pools grow once. */
    int ok = mmap_touch_unmap(0);
    struct stats before = get_stats();
    for (int i = 1; i <= 500 && ok; i++) {
        ok &= mmap_touch_unmap(i);
    }
    report("mmap_touch_munmap_500_zero_leak", ok && same_stats(before, get_stats()));
}

static void t_munmap_split(void)
{
    uint64_t more[FASTIX_CALL_MORE];
    (void)fastix_call(FASTIX_TEST_VM_STATS, 0, 0, more);
    uint64_t vmas_before = more[0];
    int64_t a = sys_mmap(0, 4 * PAGE, RW, ANON);
    int ok = a > 0;
    for (uint64_t p = 0; ok && p < 4; p++) {
        *(volatile uint64_t *)(uintptr_t)((uint64_t)a + p * PAGE) = p + 1;
    }
    ok &= sys_munmap((uint64_t)a + PAGE, PAGE) == 0;
    (void)fastix_call(FASTIX_TEST_VM_STATS, 0, 0, more);
    ok &= more[0] == vmas_before + 2; /* one VMA became two */
    for (uint64_t p = 0; ok && p < 4; p++) {
        if (p != 1) {
            ok &= *(volatile uint64_t *)(uintptr_t)((uint64_t)a + p * PAGE) == p + 1;
        }
    }
    int64_t pid;
    touched_addr = (uint64_t)a + PAGE;
    int status = in_child(child_read, &pid);
    ok &= killed_by_segv(status, pid, FASTIX_SEGV_MAPERR, touched_addr);
    ok &= sys_munmap((uint64_t)a, 4 * PAGE) == 0;
    report("munmap_splits_vma", ok);
}

static void t_null_deref(void)
{
    int64_t pid;
    touched_addr = NULL_ADDR;
    int status = in_child(child_read, &pid);
    report("null_deref_sigsegv", killed_by_segv(status, pid, FASTIX_SEGV_MAPERR, NULL_ADDR));
}

static void t_mprotect(void)
{
    int64_t a = sys_mmap(0, PAGE, RW, ANON), pid;
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)a;
    int ok = a > 0;
    p[0] = 7;
    ok &= sys_mprotect((uint64_t)a, PAGE, FASTIX_PROT_READ) == 0 && p[0] == 7;
    touched_addr = (uint64_t)a;
    int status = in_child(child_write, &pid);
    ok &= killed_by_segv(status, pid, FASTIX_SEGV_ACCERR, (uint64_t)a);
    ok &= sys_mprotect((uint64_t)a, PAGE, RW) == 0;
    p[0] = 8;
    ok &= p[0] == 8 && sys_munmap((uint64_t)a, PAGE) == 0;
    report("mprotect_readonly_write_sigsegv", ok);
}

static volatile int recursion_limit = 1 << 30;

static int recurse(int depth)
{
    volatile uint8_t frame[65536];
    frame[0] = (uint8_t)depth;
    frame[sizeof(frame) - 1] = (uint8_t)depth;
    if (depth >= recursion_limit) {
        return 0;
    }
    return recurse(depth + 1) + frame[100];
}

static void child_overflow_stack(void)
{
    test_arg = recurse(0);
}

static void t_stack_guard(void)
{
    int64_t pid;
    uint64_t more[FASTIX_CALL_MORE], args[1];
    uint64_t stack_bottom = FASTIX_STACK_TOP - FASTIX_DEFAULT_STACK_SIZE;
    int status = in_child(child_overflow_stack, &pid);
    args[0] = (uint64_t)pid;
    int ok = killed_by_segv(status, pid, FASTIX_SEGV_MAPERR, 0) &&
             fastix_call(FASTIX_TEST_LAST_KILL, 1, args, more) == 0 &&
             more[2] < stack_bottom && more[2] >= stack_bottom - 2 * 65536;
    report("stack_overflow_guard_sigsegv", ok);
}

static uint64_t cow_page;

static void child_cow(void)
{
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)cow_page;
    if (p[0] != 'A') {
        sys_exit(1);
    }
    p[0] = 'B';
    sys_exit(p[0] == 'B' ? 0 : 2);
}

static void t_cow(void)
{
    int64_t a = sys_mmap(0, PAGE, RW, ANON);
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)a;
    int ok = a > 0;
    p[0] = 'A';
    cow_page = (uint64_t)a;
    int status = in_child(child_cow, 0);
    ok &= status == FASTIX_W_EXITED(0) && p[0] == 'A'; /* child's write stayed private */

    /* Child gone: the parent owns the frame alone, so writing must not copy it. */
    uint64_t frames_before = get_stats().v[2];
    p[0] = 'C';
    ok &= p[0] == 'C' && get_stats().v[2] == frames_before;
    ok &= sys_munmap((uint64_t)a, PAGE) == 0;
    report("cow_fork_private_and_sole_owner_upgrade", ok);
}

static void child_exit_with_arg(void)
{
    sys_exit(test_arg);
}

static void t_fork_100(void)
{
    int ok = 1;
    test_arg = 0;
    (void)in_child(child_exit_with_arg, 0); /* warm-up: procd's caches reach steady size */
    struct stats before = get_stats();
    for (int i = 0; i < 100 && ok; i++) {
        int64_t pid;
        if (i % 2 == 0) {
            test_arg = i;
            ok &= in_child(child_exit_with_arg, &pid) == FASTIX_W_EXITED(i);
        } else {
            touched_addr = NULL_ADDR;
            ok &= killed_by_segv(in_child(child_read, &pid), pid, FASTIX_SEGV_MAPERR, NULL_ADDR);
        }
    }
    report("fork_exit_fault_wait_100_zero_leak", ok && same_stats(before, get_stats()));
}

static void t_brk(void)
{
    int64_t pid;
    int64_t b0 = fastix_sys1(FASTIX_SYS_brk, 0);
    uint64_t want = (uint64_t)b0 + 3 * PAGE + 100;
    int ok = b0 > 0 && fastix_sys1(FASTIX_SYS_brk, want) == (int64_t)want;
    for (uint64_t addr = (uint64_t)b0; ok && addr < want; addr += 512) {
        *(volatile uint8_t *)(uintptr_t)addr = 0x5A;
    }
    ok &= fastix_sys1(FASTIX_SYS_brk, (uint64_t)b0) == b0;
    touched_addr = (uint64_t)b0 + PAGE;
    ok &= killed_by_segv(in_child(child_read, &pid), pid, FASTIX_SEGV_MAPERR, touched_addr);
    ok &= fastix_sys1(FASTIX_SYS_brk, 0) == b0;
    report("brk_grow_shrink", ok);
}

static void t_errors(void)
{
    int status;
    int64_t a = sys_mmap(0, PAGE, RW, ANON);
    int ok = a > 0;
    ok &= sys_mmap(0, 0, RW, ANON) == -FASTIX_EINVAL;
    ok &= sys_mmap(0, PAGE, RW | FASTIX_PROT_EXEC, ANON) == -FASTIX_EACCES;            /* W^X */
    ok &= sys_mprotect((uint64_t)a, PAGE, RW | FASTIX_PROT_EXEC) == -FASTIX_EACCES;    /* W^X */
    ok &= sys_mmap((uint64_t)a + 1, PAGE, RW, ANON | FASTIX_MAP_FIXED) == -FASTIX_EINVAL;
    ok &= sys_mmap((uint64_t)a, PAGE, RW, ANON | FASTIX_MAP_FIXED_NOREPLACE) == -FASTIX_EEXIST;
    ok &= sys_mmap(0, PAGE, RW, FASTIX_MAP_ANONYMOUS) == -FASTIX_EINVAL; /* missing PRIVATE/SHARED */
    ok &= sys_munmap((uint64_t)a + 1, PAGE) == -FASTIX_EINVAL;
    ok &= sys_munmap(FASTIX_VDSO_ADDR, PAGE) == -FASTIX_EINVAL;  /* procd's pages are fixed */
    ok &= sys_munmap((uint64_t)a, PAGE) == 0;
    ok &= sys_mprotect((uint64_t)a, PAGE, FASTIX_PROT_READ) == -FASTIX_ENOMEM; /* unmapped */
    ok &= sys_wait(-1, &status) == -FASTIX_ECHILD;
    ok &= fastix_sys0(0x7FF) == -FASTIX_ENOSYS;
    report("syscall_error_codes", ok);
}

static void t_spawn(const char *name, uint64_t image)
{
    int status = -1;
    int64_t pid = fastix_sys1(FASTIX_TEST_SPAWN, image);
    report(name, pid > 0 && sys_wait(pid, &status) == pid && status == FASTIX_W_EXITED(HELLO_OK));
}

_Noreturn void prog_main(uint64_t *sp)
{
    (void)sp;
    fastix_sys_init();
    kprint_str("vmtest: started as PID 1\n");
    t_ipc_100k();
    t_vdso();
    t_mmap_500();
    t_munmap_split();
    t_null_deref();
    t_mprotect();
    t_stack_guard();
    t_cow();
    t_fork_100();
    t_brk();
    t_errors();
    t_spawn("load_static_elf_sysv_stack", IMAGE_STATIC);
    t_spawn("load_static_pie_sysv_stack", IMAGE_PIE);
    run_proc_tests();
    run_sig_tests();
    run_thread_tests();
    run_pipe_tests();
    kprint_str("vmtest: failures=");
    kprint_u64((uint64_t)failures);
    kprint_str("\n");
    sys_exit(failures);
}
