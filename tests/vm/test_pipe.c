/*
 * test_pipe.c - Phase 3D behavioral tests: pipe2 (EOF, EPIPE + SIGPIPE,
 * O_NONBLOCK, dup/fcntl, zero-leak), pipe throughput (4 MB stream), atomic
 * 4 KB PIPE_BUF multi-writer stress (4 concurrent writers), and eventfd2
 * (normal, blocking, EFD_SEMAPHORE, O_NONBLOCK).
 */
#include <stdatomic.h>
#include <stdint.h>

#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <fastix/futex_abi.h>
#include <fastix/pipe_abi.h>
#include <fastix/signal_abi.h>
#include <fastix/time_abi.h>
#include <fastix_sys/sys.h>
#include <libos/kprint.h>

#include "vmtest.h"

static int64_t sys_pipe2(int32_t fds[2], uint32_t flags)
{
    return fastix_sys2(FASTIX_SYS_pipe2, (uint64_t)(uintptr_t)fds, flags);
}

static int64_t sys_read(int32_t fd, void *buf, uint64_t len)
{
    return fastix_fd_read(fd, buf, len);
}

static int64_t sys_write(int32_t fd, const void *buf, uint64_t len)
{
    return fastix_fd_write(fd, buf, len);
}

static int64_t sys_close(int32_t fd)
{
    return fastix_vfs1(FASTIX_SYS_close, (uint64_t)(int64_t)fd);
}

/* ---- 1. Basic pipe2, EOF, EPIPE + SIGPIPE, O_NONBLOCK, dup/fcntl, zero leak ---- */

static volatile int sigpipe_hits;

static void handle_sigpipe(int sig)
{
    if (sig == FASTIX_SIGPIPE) {
        sigpipe_hits++;
    }
}

static uint8_t pipe_scratch[FASTIX_PIPE_RING_SIZE];

static int run_pipe_basic_once(void)
{
    int ok = 1, st = 0;
    int32_t fds[2] = { -1, -1 };

    ok &= sys_pipe2(fds, FASTIX_O_NONBLOCK) == 0 && fds[0] >= 0 && fds[1] >= 0;
    ok &= sys_read(fds[0], pipe_scratch, 16) == -FASTIX_EAGAIN;

    /* Fill all 64 KB - 2048 bytes, then a 4096-byte atomic write must fail -EAGAIN. */
    uint32_t fill_sz = FASTIX_PIPE_RING_SIZE - 2048u;
    for (uint32_t i = 0; i < fill_sz; i++) {
        pipe_scratch[i] = (uint8_t)(i & 0xffu);
    }
    ok &= sys_write(fds[1], pipe_scratch, fill_sz) == (int64_t)fill_sz;
    ok &= sys_write(fds[1], pipe_scratch, FASTIX_PIPE_BUF) == -FASTIX_EAGAIN;
    /* A 2048-byte write fits exactly; then pipe is full and 1-byte write fails -EAGAIN. */
    ok &= sys_write(fds[1], pipe_scratch, 2048) == 2048;
    ok &= sys_write(fds[1], pipe_scratch, 1) == -FASTIX_EAGAIN;

    /* Drain and verify data, then close write end -> EOF (0). */
    uint32_t total = 0;
    while (total < FASTIX_PIPE_RING_SIZE) {
        int64_t n = sys_read(fds[0], pipe_scratch, sizeof(pipe_scratch));
        if (n <= 0) {
            ok = 0;
            break;
        }
        total += (uint32_t)n;
    }
    ok &= total == FASTIX_PIPE_RING_SIZE;
    ok &= sys_close(fds[1]) == 0;
    ok &= sys_read(fds[0], pipe_scratch, 16) == 0; /* EOF */
    ok &= sys_close(fds[0]) == 0;

    /* EPIPE + caught SIGPIPE. */
    ok &= sys_pipe2(fds, 0) == 0;
    struct fastix_sigaction sa = { .handler = (uint64_t)(uintptr_t)&handle_sigpipe };
    ok &= fastix_sys3(FASTIX_SYS_rt_sigaction, FASTIX_SIGPIPE, (uint64_t)(uintptr_t)&sa, 0) == 0;
    sigpipe_hits = 0;
    ok &= sys_close(fds[0]) == 0;
    ok &= sys_write(fds[1], "x", 1) == -FASTIX_EPIPE && sigpipe_hits == 1;
    ok &= sys_close(fds[1]) == 0;
    struct fastix_sigaction dfl = { .handler = FASTIX_SIG_DFL };
    ok &= fastix_sys3(FASTIX_SYS_rt_sigaction, FASTIX_SIGPIPE, (uint64_t)(uintptr_t)&dfl, 0) == 0;

    /* Default SIGPIPE terminates child. */
    ok &= sys_pipe2(fds, 0) == 0;
    int64_t c = fastix_sys0(FASTIX_SYS_fork);
    if (c == 0) {
        (void)sys_close(fds[0]);
        (void)sys_write(fds[1], "boom", 4);
        vmtest_exit(0);
    }
    ok &= sys_close(fds[0]) == 0 && sys_close(fds[1]) == 0;
    ok &= c > 0 && vmtest_wait4(c, &st, 0) == c && st == FASTIX_W_SIGNALED(FASTIX_SIGPIPE);

    /* dup & fcntl F_GETFL/F_SETFL/F_GETFD/F_SETFD */
    ok &= sys_pipe2(fds, FASTIX_O_CLOEXEC) == 0;
    ok &= fastix_vfs3(FASTIX_SYS_fcntl, (uint64_t)fds[0], FASTIX_F_GETFD, 0) == FASTIX_FD_CLOEXEC;
    int32_t d = (int32_t)fastix_vfs1(FASTIX_SYS_dup, (uint64_t)fds[0]);
    ok &= d >= 0 && fastix_vfs3(FASTIX_SYS_fcntl, (uint64_t)d, FASTIX_F_GETFD, 0) == 0;
    ok &= fastix_vfs3(FASTIX_SYS_fcntl, (uint64_t)d, FASTIX_F_SETFL, FASTIX_O_NONBLOCK) == 0;
    ok &= fastix_vfs3(FASTIX_SYS_fcntl, (uint64_t)fds[0], FASTIX_F_GETFL, 0) == FASTIX_O_NONBLOCK;
    ok &= sys_close(d) == 0 && sys_close(fds[0]) == 0 && sys_close(fds[1]) == 0;
    return ok;
}

static void t_pipe_basic(void)
{
    /* Warm up pipe slab caches & procd pipe page tables once, then check 0 leaks. */
    int ok = run_pipe_basic_once();
    struct vm_test_stats before = vmtest_get_stats();
    for (int i = 0; i < 3 && ok; i++) {
        ok &= run_pipe_basic_once();
    }
    ok &= vmtest_same_stats(before, vmtest_get_stats());
    vmtest_report("pipe_eof_epipe_sigpipe_nonblock_zero_leak", ok);
}

/* ---- 2. Pipe throughput (4 MB) & 4-writer atomic 4 KB PIPE_BUF stress ---- */

#define ATOMIC_WRITERS 4
#define ATOMIC_CHUNKS  64 /* 4 * 64 * 4096 = 1 MB total */
#define ODD_READ       1000 /* reader piece size (not a divisor of PIPE_BUF) */
#define W_STACK_SIZE   16384

static uint8_t writer_stacks[ATOMIC_WRITERS][W_STACK_SIZE] __attribute__((aligned(16)));
static volatile uint32_t writer_ctids[ATOMIC_WRITERS];
static int32_t stress_wfd;

static void atomic_writer(uint64_t id)
{
    uint8_t blk[FASTIX_PIPE_BUF];
    for (uint32_t c = 0; c < ATOMIC_CHUNKS; c++) {
        uint8_t tag = (uint8_t)(0x40u + (id << 4) + (c & 0x0Fu));
        for (uint32_t i = 0; i < FASTIX_PIPE_BUF; i++) {
            blk[i] = tag;
        }
        int64_t w = sys_write(stress_wfd, blk, FASTIX_PIPE_BUF);
        if (w != FASTIX_PIPE_BUF) {
            break;
        }
    }
    (void)fastix_sys1(FASTIX_SYS_exit, 0);
    for (;;) {
    }
}

static void t_pipe_throughput_and_atomic_4kb(void)
{
    int ok = 1, st = 0;
    int32_t fds[2] = { -1, -1 };

    /* A. 4 MB throughput stream between parent and child (64 x 64 KB). */
    ok &= sys_pipe2(fds, 0) == 0;
    const uint32_t nchunks = 64;
    const uint64_t total_bytes = (uint64_t)nchunks * FASTIX_PIPE_RING_SIZE;
    uint64_t t0 = fastix_clock_mono_ns();

    int64_t prod = fastix_sys0(FASTIX_SYS_fork);
    if (prod == 0) {
        (void)sys_close(fds[0]);
        for (uint32_t c = 0; c < nchunks; c++) {
            for (uint32_t i = 0; i < FASTIX_PIPE_RING_SIZE; i += 64) {
                pipe_scratch[i] = (uint8_t)((c + i) & 0xffu);
            }
            if (sys_write(fds[1], pipe_scratch, FASTIX_PIPE_RING_SIZE) != FASTIX_PIPE_RING_SIZE) {
                vmtest_exit(1);
            }
        }
        (void)sys_close(fds[1]);
        vmtest_exit(0);
    }
    ok &= sys_close(fds[1]) == 0;
    uint64_t got_total = 0;
    while (got_total < total_bytes) {
        int64_t n = sys_read(fds[0], pipe_scratch, sizeof(pipe_scratch));
        if (n <= 0) {
            ok = 0;
            break;
        }
        got_total += (uint64_t)n;
    }
    ok &= sys_read(fds[0], pipe_scratch, 16) == 0; /* EOF after 4 MB */
    ok &= sys_close(fds[0]) == 0;
    ok &= prod > 0 && vmtest_wait4(prod, &st, 0) == prod && st == FASTIX_W_EXITED(0);

    uint64_t dt_ns = fastix_clock_mono_ns() - t0;
    if (dt_ns > 0) {
        uint64_t mibs = (total_bytes * 1000ULL) / dt_ns; /* MB/s (approx) */
        kprint_str("  pipe 4 MiB stream: ");
        kprint_u64(mibs);
        kprint_str(" MB/s (QEMU TCG, not a benchmark)\n");
    }

    /* B. Atomic 4 KB PIPE_BUF stress: 4 concurrent writers, 0 torn/interleaved blocks. */
    ok &= sys_pipe2(fds, 0) == 0;
    stress_wfd = fds[1];
    for (int i = 0; i < ATOMIC_WRITERS; i++) {
        writer_ctids[i] = 1;
        int64_t tid = fastix_sys6(FASTIX_SYS_thread_create, (uint64_t)(uintptr_t)&atomic_writer,
                                  (uint64_t)(uintptr_t)(writer_stacks[i] + W_STACK_SIZE),
                                  (uint64_t)i, 0, (uint64_t)(uintptr_t)&writer_ctids[i],
                                  FASTIX_CLONE_CHILD_SETTID | FASTIX_CLONE_CHILD_CLEARTID);
        ok &= tid > 0;
    }

    uint32_t per_writer[ATOMIC_WRITERS] = { 0 };
    uint32_t total_blocks = ATOMIC_WRITERS * ATOMIC_CHUNKS;
    uint8_t blk[FASTIX_PIPE_BUF];
    for (uint32_t b = 0; b < total_blocks && ok; b++) {
        uint32_t off = 0;
        while (off < FASTIX_PIPE_BUF) {
            /* Read in uneven pieces so writers often find less than PIPE_BUF free.
             * (Tearing is also prevented by procd resuming parked writers in order;
             * the non-blocking EAGAIN check in the test above covers the atomic rule.) */
            uint32_t want = FASTIX_PIPE_BUF - off < ODD_READ ? FASTIX_PIPE_BUF - off : ODD_READ;
            int64_t n = sys_read(fds[0], blk + off, want);
            if (n <= 0) {
                ok = 0;
                break;
            }
            off += (uint32_t)n;
        }
        uint8_t tag = blk[0];
        uint32_t wid = (tag - 0x40u) >> 4;
        if (tag < 0x40u || wid >= ATOMIC_WRITERS) {
            ok = 0;
            break;
        }
        for (uint32_t i = 1; i < FASTIX_PIPE_BUF; i++) {
            if (blk[i] != tag) {
                ok = 0; /* torn/interleaved PIPE_BUF write detected! */
                break;
            }
        }
        per_writer[wid]++;
    }
    for (int i = 0; i < ATOMIC_WRITERS; i++) {
        while (__atomic_load_n(&writer_ctids[i], __ATOMIC_ACQUIRE) != 0) {
            uint32_t v = __atomic_load_n(&writer_ctids[i], __ATOMIC_ACQUIRE);
            (void)fastix_sys6(FASTIX_SYS_futex, (uint64_t)(uintptr_t)&writer_ctids[i],
                              FASTIX_FUTEX_WAIT | FASTIX_FUTEX_PRIVATE_FLAG, v, 0, 0, 0);
        }
        ok &= per_writer[i] == ATOMIC_CHUNKS;
    }
    ok &= sys_close(fds[1]) == 0;
    ok &= sys_read(fds[0], blk, 16) == 0; /* EOF */
    ok &= sys_close(fds[0]) == 0;

    vmtest_report("pipe_throughput_and_atomic_4kb_stress", ok);
}

/* ---- 3. eventfd2 ---- */

static int run_eventfd_once(void)
{
    int ok = 1, st = 0;
    uint64_t val = 0;

    /* A. Normal non-blocking eventfd2 */
    int32_t efd = (int32_t)fastix_sys2(FASTIX_SYS_eventfd2, 3, FASTIX_EFD_NONBLOCK);
    ok &= efd >= 0;
    ok &= sys_read(efd, &val, 4) == -FASTIX_EINVAL;
    ok &= sys_read(efd, &val, 8) == 8 && val == 3;
    ok &= sys_read(efd, &val, 8) == -FASTIX_EAGAIN;
    uint64_t bad = UINT64_MAX;
    ok &= sys_write(efd, &bad, 8) == -FASTIX_EINVAL;
    val = 5;
    ok &= sys_write(efd, &val, 8) == 8;
    val = 7;
    ok &= sys_write(efd, &val, 8) == 8;
    val = 0;
    ok &= sys_read(efd, &val, 8) == 8 && val == 12;
    ok &= sys_close(efd) == 0;

    /* B. Blocking eventfd2 across fork */
    efd = (int32_t)fastix_sys2(FASTIX_SYS_eventfd2, 0, 0);
    ok &= efd >= 0;
    int64_t c = fastix_sys0(FASTIX_SYS_fork);
    if (c == 0) {
        uint64_t got = 0;
        int64_t r = sys_read(efd, &got, 8);
        vmtest_exit(r == 8 && got == 42 ? 0 : 1);
    }
    struct fastix_timespec sl = { 0, 10000000 };
    (void)fastix_sys2(FASTIX_SYS_nanosleep, (uint64_t)(uintptr_t)&sl, 0);
    val = 42;
    ok &= sys_write(efd, &val, 8) == 8;
    ok &= c > 0 && vmtest_wait4(c, &st, 0) == c && st == FASTIX_W_EXITED(0);
    ok &= sys_close(efd) == 0;

    /* C. EFD_SEMAPHORE decrements by 1 per read */
    efd = (int32_t)fastix_sys2(FASTIX_SYS_eventfd2, 3,
                               FASTIX_EFD_SEMAPHORE | FASTIX_EFD_NONBLOCK);
    ok &= efd >= 0;
    for (int i = 0; i < 3; i++) {
        val = 0;
        ok &= sys_read(efd, &val, 8) == 8 && val == 1;
    }
    ok &= sys_read(efd, &val, 8) == -FASTIX_EAGAIN;
    ok &= sys_close(efd) == 0;
    return ok;
}

static void t_eventfd(void)
{
    int ok = run_eventfd_once();
    struct vm_test_stats before = vmtest_get_stats();
    ok &= run_eventfd_once();
    ok &= vmtest_same_stats(before, vmtest_get_stats());
    vmtest_report("eventfd2_blocking_nonblock_semaphore", ok);
}

void run_pipe_tests(void)
{
    t_pipe_basic();
    t_pipe_throughput_and_atomic_4kb();
    t_eventfd();
}
