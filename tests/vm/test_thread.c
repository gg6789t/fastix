/*
 * test_thread.c - Phase 3C behavioral tests: thread_create + TLS (tpidr_el0) +
 * clear_child_tid join, 8-thread 100,000-op futex mutex contention, shared
 * cross-process futex, WAIT/WAKE_BITSET, CMP_REQUEUE, timeout, robust lists,
 * multithreaded fork (keeps only calling thread), and exit vs exit_group.
 */
#include <stdatomic.h>
#include <stdint.h>

#include <arch/regs.h>
#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <fastix/futex_abi.h>
#include <fastix/mman.h>
#include <fastix/signal_abi.h>
#include <fastix/time_abi.h>
#include <fastix_sys/sys.h>

#include "vmtest.h"

#define PAGE          FASTIX_PAGE_SIZE
#define RW            (FASTIX_PROT_READ | FASTIX_PROT_WRITE)
#define THREAD_STACK  16384
#define NUM_THREADS   8
#define OPS_PER_THR   12500 /* 8 * 12,500 = 100,000 ops total */

static int64_t sys_futex(volatile uint32_t *uaddr, uint32_t op, uint32_t val, uint64_t val2,
                         volatile uint32_t *uaddr2, uint32_t val3)
{
    return fastix_sys6(FASTIX_SYS_futex, (uint64_t)(uintptr_t)uaddr, op, val, val2,
                       (uint64_t)(uintptr_t)uaddr2, val3);
}

static int64_t sys_thread_create(void (*fn)(uint64_t), void *stack_top, uint64_t arg, uint64_t tls,
                                 volatile uint32_t *ctid, uint32_t flags)
{
    return fastix_sys6(FASTIX_SYS_thread_create, (uint64_t)(uintptr_t)fn,
                       (uint64_t)(uintptr_t)stack_top, arg, tls, (uint64_t)(uintptr_t)ctid, flags);
}

static _Noreturn void sys_thread_exit(int code)
{
    (void)fastix_sys1(FASTIX_SYS_exit, (uint64_t)code);
    for (;;) {
    }
}

static void join_ctid(volatile uint32_t *ctid)
{
    for (;;) {
        uint32_t v = __atomic_load_n(ctid, __ATOMIC_ACQUIRE);
        if (v == 0) {
            return;
        }
        (void)sys_futex(ctid, FASTIX_FUTEX_WAIT | FASTIX_FUTEX_PRIVATE_FLAG, v, 0, 0, 0);
    }
}

/* ---- 1. Thread create + TLS + clear_child_tid + zero leak ---- */

static uint8_t thr_stacks[NUM_THREADS][THREAD_STACK] __attribute__((aligned(16)));
static volatile uint32_t thr_ctids[NUM_THREADS];
static _Atomic uint32_t tls_ok_count;

static void tls_worker(uint64_t idx)
{
    uint64_t expect_tls = 0x100000ULL + idx * 0x1234ULL;
    uint64_t got_tls = arch_read_tpidr();
    int64_t tid = fastix_sys0(FASTIX_SYS_gettid);
    int64_t pid = fastix_sys0(FASTIX_SYS_getpid);

    /* Also exercise set_tls on this thread. */
    int set_ok = fastix_sys1(FASTIX_SYS_set_tls, expect_tls + 8) == 0 &&
                 arch_read_tpidr() == expect_tls + 8;

    if (got_tls == expect_tls && set_ok && tid > 0 && (uint32_t)tid == thr_ctids[idx] && pid == 1) {
        atomic_fetch_add_explicit(&tls_ok_count, 1, memory_order_relaxed);
    }
    sys_thread_exit(0);
}

static int run_tls_batch(void)
{
    int ok = 1;
    atomic_store_explicit(&tls_ok_count, 0, memory_order_relaxed);
    for (int i = 0; i < NUM_THREADS; i++) {
        thr_ctids[i] = 1;
        uint64_t tls = 0x100000ULL + (uint64_t)i * 0x1234ULL;
        int64_t tid = sys_thread_create(
            tls_worker, thr_stacks[i] + THREAD_STACK, (uint64_t)i, tls, &thr_ctids[i],
            FASTIX_CLONE_SETTLS | FASTIX_CLONE_CHILD_SETTID | FASTIX_CLONE_CHILD_CLEARTID);
        ok &= tid > 0;
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        join_ctid(&thr_ctids[i]);
    }
    ok &= atomic_load_explicit(&tls_ok_count, memory_order_acquire) == NUM_THREADS;
    return ok;
}

static void t_threads_tls_and_leak(void)
{
    /* Warm up thread_cnode & slab caches on first batch, then verify 0 leaks on batch 2. */
    int ok = run_tls_batch();
    struct vm_test_stats before = vmtest_get_stats();
    ok &= run_tls_batch();
    ok &= vmtest_same_stats(before, vmtest_get_stats());
    vmtest_report("threads_tls_cleartid_zero_leak", ok);
}

/* ---- 2. 8-thread, 100,000-op futex mutex contention ---- */

struct fmutex {
    _Atomic uint32_t state; /* 0 = unlocked, 1 = locked uncontended, 2 = locked with waiters */
};

static void fmutex_lock(struct fmutex *m)
{
    uint32_t c = 0;
    if (atomic_compare_exchange_strong_explicit(&m->state, &c, 1, memory_order_acquire,
                                                memory_order_relaxed)) {
        return;
    }
    for (;;) {
        if (c == 2 || atomic_exchange_explicit(&m->state, 2, memory_order_acquire) != 0) {
            (void)sys_futex((volatile uint32_t *)&m->state,
                            FASTIX_FUTEX_WAIT | FASTIX_FUTEX_PRIVATE_FLAG, 2, 0, 0, 0);
        }
        c = 0;
        if (atomic_compare_exchange_strong_explicit(&m->state, &c, 2, memory_order_acquire,
                                                    memory_order_relaxed)) {
            return;
        }
    }
}

static void fmutex_unlock(struct fmutex *m)
{
    if (atomic_fetch_sub_explicit(&m->state, 1, memory_order_release) != 1) {
        atomic_store_explicit(&m->state, 0, memory_order_release);
        (void)sys_futex((volatile uint32_t *)&m->state,
                        FASTIX_FUTEX_WAKE | FASTIX_FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    }
}

static struct fmutex contention_lock;
static volatile uint64_t contention_counter;

static void contention_worker(uint64_t arg)
{
    (void)arg;
    for (int i = 0; i < OPS_PER_THR; i++) {
        fmutex_lock(&contention_lock);
        contention_counter = contention_counter + 1;
        fmutex_unlock(&contention_lock);
    }
    sys_thread_exit(0);
}

static void t_futex_contention(void)
{
    int ok = 1;
    atomic_store_explicit(&contention_lock.state, 0, memory_order_relaxed);
    contention_counter = 0;

    for (int i = 0; i < NUM_THREADS; i++) {
        thr_ctids[i] = 1;
        int64_t tid = sys_thread_create(contention_worker, thr_stacks[i] + THREAD_STACK,
                                        (uint64_t)i, 0, &thr_ctids[i],
                                        FASTIX_CLONE_CHILD_SETTID | FASTIX_CLONE_CHILD_CLEARTID);
        ok &= tid > 0;
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        join_ctid(&thr_ctids[i]);
    }
    ok &= contention_counter == (uint64_t)NUM_THREADS * OPS_PER_THR;
    vmtest_report("futex_contention_8threads_100k_ops", ok);
}

/* ---- 3. Shared futex, bitset, cmp_requeue, timeout, robust lists ---- */

static volatile uint32_t bitset_word;
static volatile uint32_t bitset_order[2];
static _Atomic uint32_t bitset_pos;
static _Atomic uint32_t bitset_ready;

static void bitset_worker(uint64_t id)
{
    uint32_t mask = (id == 0) ? 0x1u : 0x2u;
    atomic_fetch_add_explicit(&bitset_ready, 1, memory_order_release);
    int64_t r = sys_futex(&bitset_word, FASTIX_FUTEX_WAIT_BITSET | FASTIX_FUTEX_PRIVATE_FLAG, 0, 0,
                          0, mask);
    if (r == 0) {
        uint32_t p = atomic_fetch_add_explicit(&bitset_pos, 1, memory_order_acq_rel);
        if (p < 2) {
            bitset_order[p] = (uint32_t)id + 1;
        }
    }
    sys_thread_exit(0);
}

static volatile uint32_t req_f1, req_f2;
static _Atomic uint32_t req_ready, req_woken;

static void requeue_worker(uint64_t arg)
{
    (void)arg;
    atomic_fetch_add_explicit(&req_ready, 1, memory_order_release);
    if (sys_futex(&req_f1, FASTIX_FUTEX_WAIT | FASTIX_FUTEX_PRIVATE_FLAG, 0, 0, 0, 0) == 0) {
        atomic_fetch_add_explicit(&req_woken, 1, memory_order_acq_rel);
    }
    sys_thread_exit(0);
}

struct robust_node {
    struct fastix_robust_list link;
    volatile uint32_t lock;
};

static struct robust_node rob_node;
static _Atomic uint32_t rob_locked;

static void robust_dying_worker(uint64_t arg)
{
    (void)arg;
    struct fastix_robust_list_head head;
    head.list.next = (uint64_t)(uintptr_t)&rob_node.link;
    head.futex_offset = (int64_t)((uintptr_t)&rob_node.lock - (uintptr_t)&rob_node.link);
    head.list_op_pending = 0;
    rob_node.link.next = (uint64_t)(uintptr_t)&head;

    uint32_t mytid = (uint32_t)fastix_sys0(FASTIX_SYS_gettid);
    (void)fastix_sys2(FASTIX_SYS_set_robust_list, (uint64_t)(uintptr_t)&head, sizeof(head));
    __atomic_store_n(&rob_node.lock, mytid | FASTIX_FUTEX_WAITERS, __ATOMIC_RELEASE);
    atomic_store_explicit(&rob_locked, 1, memory_order_release);
    /* Wait briefly so main thread parks in FUTEX_WAIT on rob_node.lock, then die holding it! */
    struct fastix_timespec ts = { 0, 15000000 };
    (void)fastix_sys2(FASTIX_SYS_nanosleep, (uint64_t)(uintptr_t)&ts, 0);
    sys_thread_exit(0);
}

static void t_futex_features(void)
{
    int ok = 1, st = 0;

    /* A. Timeout returns -ETIMEDOUT */
    volatile uint32_t tword = 5;
    struct fastix_timespec to = { 0, 15000000 }; /* 15 ms */
    ok &= sys_futex(&tword, FASTIX_FUTEX_WAIT | FASTIX_FUTEX_PRIVATE_FLAG, 4, 0, 0, 0) ==
          -FASTIX_EAGAIN;
    ok &= sys_futex(&tword, FASTIX_FUTEX_WAIT | FASTIX_FUTEX_PRIVATE_FLAG, 5,
                    (uint64_t)(uintptr_t)&to, 0, 0) == -FASTIX_ETIMEDOUT;

    /* B. Shared cross-process futex over MAP_SHARED */
    int64_t shm = fastix_sys6(FASTIX_SYS_mmap, 0, PAGE, RW,
                              FASTIX_MAP_SHARED | FASTIX_MAP_ANONYMOUS, (uint64_t)-1, 0);
    ok &= shm > 0;
    volatile uint32_t *sf = (volatile uint32_t *)(uintptr_t)shm;
    *sf = 0;
    int64_t c = fastix_sys0(FASTIX_SYS_fork);
    if (c == 0) {
        int64_t r = sys_futex(sf, FASTIX_FUTEX_WAIT, 0, 0, 0, 0); /* shared (no PRIVATE_FLAG) */
        vmtest_exit(r == 0 && *sf == 1 ? 0 : 1);
    }
    struct fastix_timespec sl = { 0, 10000000 };
    (void)fastix_sys2(FASTIX_SYS_nanosleep, (uint64_t)(uintptr_t)&sl, 0);
    __atomic_store_n(sf, 1, __ATOMIC_RELEASE);
    ok &= sys_futex(sf, FASTIX_FUTEX_WAKE, 1, 0, 0, 0) == 1;
    ok &= vmtest_wait4(c, &st, 0) == c && st == FASTIX_W_EXITED(0);
    ok &= fastix_sys2(FASTIX_SYS_munmap, (uint64_t)shm, PAGE) == 0;

    /* C. WAIT_BITSET / WAKE_BITSET selective wakeup */
    bitset_word = 0;
    bitset_order[0] = bitset_order[1] = 0;
    atomic_store_explicit(&bitset_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&bitset_ready, 0, memory_order_relaxed);
    for (int i = 0; i < 2; i++) {
        thr_ctids[i] = 1;
        ok &= sys_thread_create(bitset_worker, thr_stacks[i] + THREAD_STACK, (uint64_t)i, 0,
                                &thr_ctids[i],
                                FASTIX_CLONE_CHILD_SETTID | FASTIX_CLONE_CHILD_CLEARTID) > 0;
    }
    while (atomic_load_explicit(&bitset_ready, memory_order_acquire) < 2) {
        (void)fastix_sys0(FASTIX_SYS_sched_yield);
    }
    (void)fastix_sys2(FASTIX_SYS_nanosleep, (uint64_t)(uintptr_t)&sl, 0);
    /* Wake bit 0x2 first (worker 1), then bit 0x1 (worker 0). */
    ok &= sys_futex(&bitset_word, FASTIX_FUTEX_WAKE_BITSET | FASTIX_FUTEX_PRIVATE_FLAG, 1, 0, 0,
                    0x2u) == 1;
    join_ctid(&thr_ctids[1]);
    ok &= sys_futex(&bitset_word, FASTIX_FUTEX_WAKE_BITSET | FASTIX_FUTEX_PRIVATE_FLAG, 1, 0, 0,
                    0x1u) == 1;
    join_ctid(&thr_ctids[0]);
    ok &= bitset_order[0] == 2 && bitset_order[1] == 1;

    /* D. CMP_REQUEUE moves remaining waiters from f1 to f2 */
    req_f1 = 0;
    req_f2 = 0;
    atomic_store_explicit(&req_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&req_woken, 0, memory_order_relaxed);
    for (int i = 0; i < 3; i++) {
        thr_ctids[i] = 1;
        ok &= sys_thread_create(requeue_worker, thr_stacks[i] + THREAD_STACK, (uint64_t)i, 0,
                                &thr_ctids[i],
                                FASTIX_CLONE_CHILD_SETTID | FASTIX_CLONE_CHILD_CLEARTID) > 0;
    }
    while (atomic_load_explicit(&req_ready, memory_order_acquire) < 3) {
        (void)fastix_sys0(FASTIX_SYS_sched_yield);
    }
    (void)fastix_sys2(FASTIX_SYS_nanosleep, (uint64_t)(uintptr_t)&sl, 0);
    ok &= sys_futex(&req_f1, FASTIX_FUTEX_CMP_REQUEUE | FASTIX_FUTEX_PRIVATE_FLAG, 1, 2, &req_f2,
                    99) == -FASTIX_EAGAIN;
    ok &= sys_futex(&req_f1, FASTIX_FUTEX_CMP_REQUEUE | FASTIX_FUTEX_PRIVATE_FLAG, 1, 2, &req_f2,
                    0) == 3; /* 1 woken + 2 requeued */
    ok &= sys_futex(&req_f1, FASTIX_FUTEX_WAKE | FASTIX_FUTEX_PRIVATE_FLAG, 10, 0, 0, 0) == 0;
    ok &= sys_futex(&req_f2, FASTIX_FUTEX_WAKE | FASTIX_FUTEX_PRIVATE_FLAG, 10, 0, 0, 0) == 2;
    for (int i = 0; i < 3; i++) {
        join_ctid(&thr_ctids[i]);
    }
    ok &= atomic_load_explicit(&req_woken, memory_order_acquire) == 3;

    /* E. Robust list sets OWNER_DIED and wakes waiter when lock holder exits */
    rob_node.lock = 0;
    atomic_store_explicit(&rob_locked, 0, memory_order_relaxed);
    thr_ctids[0] = 1;
    int64_t rtid = sys_thread_create(robust_dying_worker, thr_stacks[0] + THREAD_STACK, 0, 0,
                                     &thr_ctids[0],
                                     FASTIX_CLONE_CHILD_SETTID | FASTIX_CLONE_CHILD_CLEARTID);
    ok &= rtid > 0;
    while (atomic_load_explicit(&rob_locked, memory_order_acquire) == 0) {
        (void)fastix_sys0(FASTIX_SYS_sched_yield);
    }
    uint64_t got_head = 0, got_len = 0;
    ok &= fastix_sys3(FASTIX_SYS_get_robust_list, (uint64_t)rtid, (uint64_t)(uintptr_t)&got_head,
                      (uint64_t)(uintptr_t)&got_len) == 0 &&
          got_head != 0 && got_len == sizeof(struct fastix_robust_list_head);
    uint32_t expected = (uint32_t)rtid | FASTIX_FUTEX_WAITERS;
    (void)sys_futex(&rob_node.lock, FASTIX_FUTEX_WAIT | FASTIX_FUTEX_PRIVATE_FLAG, expected, 0, 0,
                    0);
    uint32_t after = __atomic_load_n(&rob_node.lock, __ATOMIC_ACQUIRE);
    ok &= (after & FASTIX_FUTEX_OWNER_DIED) != 0 && (after & FASTIX_FUTEX_TID_MASK) == 0;
    join_ctid(&thr_ctids[0]);

    vmtest_report("futex_shared_bitset_requeue_timeout_robust", ok);
}

/* ---- 4. Multithreaded fork (only caller survives) & exit_group ---- */

static _Atomic uint32_t spin_ticks;
static _Atomic uint32_t spin_stop;

static void spinner_worker(uint64_t arg)
{
    (void)arg;
    while (atomic_load_explicit(&spin_stop, memory_order_acquire) == 0) {
        atomic_fetch_add_explicit(&spin_ticks, 1, memory_order_relaxed);
        (void)fastix_sys0(FASTIX_SYS_sched_yield);
    }
    sys_thread_exit(0);
}

static void t_mt_fork_and_exit_group(void)
{
    int ok = 1, st = 0;

    /* A. Multithreaded fork keeps ONLY calling thread in child. */
    atomic_store_explicit(&spin_ticks, 0, memory_order_relaxed);
    atomic_store_explicit(&spin_stop, 0, memory_order_relaxed);
    for (int i = 0; i < 2; i++) {
        thr_ctids[i] = 1;
        ok &= sys_thread_create(spinner_worker, thr_stacks[i] + THREAD_STACK, (uint64_t)i, 0,
                                &thr_ctids[i],
                                FASTIX_CLONE_CHILD_SETTID | FASTIX_CLONE_CHILD_CLEARTID) > 0;
    }
    while (atomic_load_explicit(&spin_ticks, memory_order_acquire) < 5) {
        (void)fastix_sys0(FASTIX_SYS_sched_yield);
    }

    int64_t child = fastix_sys0(FASTIX_SYS_fork);
    if (child == 0) {
        /* In child: spinners MUST NOT exist, so spin_ticks stays frozen across yields. */
        uint32_t snap = atomic_load_explicit(&spin_ticks, memory_order_acquire);
        for (int i = 0; i < 30; i++) {
            (void)fastix_sys0(FASTIX_SYS_sched_yield);
        }
        uint32_t snap2 = atomic_load_explicit(&spin_ticks, memory_order_acquire);
        vmtest_exit(snap == snap2 ? 66 : 1);
    }
    ok &= child > 0 && vmtest_wait4(child, &st, 0) == child && st == FASTIX_W_EXITED(66);

    atomic_store_explicit(&spin_stop, 1, memory_order_release);
    for (int i = 0; i < 2; i++) {
        join_ctid(&thr_ctids[i]);
    }

    /* B. exit_group kills all threads of a process cleanly. */
    int64_t grp = fastix_sys0(FASTIX_SYS_fork);
    if (grp == 0) {
        atomic_store_explicit(&spin_stop, 0, memory_order_relaxed);
        for (int i = 0; i < 2; i++) {
            thr_ctids[i] = 1;
            (void)sys_thread_create(spinner_worker, thr_stacks[i] + THREAD_STACK, (uint64_t)i, 0,
                                    &thr_ctids[i],
                                    FASTIX_CLONE_CHILD_SETTID | FASTIX_CLONE_CHILD_CLEARTID);
        }
        (void)fastix_sys0(FASTIX_SYS_sched_yield);
        vmtest_exit(73); /* calls FASTIX_SYS_exit_group */
    }
    ok &= grp > 0 && vmtest_wait4(grp, &st, 0) == grp && st == FASTIX_W_EXITED(73);

    vmtest_report("threads_mt_fork_and_exit_group", ok);
}

void run_thread_tests(void)
{
    t_threads_tls_and_leak();
    t_futex_contention();
    t_futex_features();
    t_mt_fork_and_exit_group();
}
