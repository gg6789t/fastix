/*
 * test_ring.c - randomized model test for libos/ring (200 seeds, ASan+UBSan).
 * Checks every write/read/atomic_all/wait-arm/EOF/EPIPE transition against a
 * simple linear byte-FIFO reference model.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fastix/errno.h>
#include <libos/ring.h>

#include "rng.h"

#define SEEDS     200
#define OPS       600
#define MAX_CAP   4096

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            fprintf(stderr, "FAIL seed=%u op=%u line=%d: %s\n", seed, op, __LINE__, #cond); \
            exit(1);                                                                        \
        }                                                                                   \
    } while (0)

struct model {
    uint8_t buf[MAX_CAP];
    uint32_t cap;
    uint32_t head;
    uint32_t tail;
    bool closed_w;
    bool closed_r;
};

static uint32_t model_used(const struct model *m)
{
    return m->head - m->tail;
}

static uint32_t model_free(const struct model *m)
{
    return m->cap - model_used(m);
}

static void test_init_errors(void)
{
    uint32_t seed = 0, op = 0;
    struct fastix_ring_hdr hdr;
    CHECK(ring_init(NULL, 64, 1, 0) == -FASTIX_EINVAL);
    CHECK(ring_init(&hdr, 0, 1, 0) == -FASTIX_EINVAL);
    CHECK(ring_init(&hdr, 1, 1, 0) == -FASTIX_EINVAL);
    CHECK(ring_init(&hdr, 3, 1, 0) == -FASTIX_EINVAL);
    CHECK(ring_init(&hdr, 100, 1, 0) == -FASTIX_EINVAL);
    CHECK(ring_init(&hdr, 64, 0, 0) == -FASTIX_EINVAL);
}

/* A peer that corrupts head/tail must not make the ring copy outside its buffer. */
static void test_corrupt_counters(void)
{
    uint32_t seed = 0, op = 0;
    static _Alignas(64) uint8_t mem[FASTIX_RING_DATA_OFFSET + 64];
    struct fastix_ring_hdr *r = (struct fastix_ring_hdr *)(void *)mem;
    uint8_t buf[256];
    bool notify;

    CHECK(ring_init(r, 64, 1, 0) == 0);
    atomic_store(&r->head, 1000); /* 1000 bytes "available" in a 64-byte ring */
    CHECK(ring_read(r, buf, sizeof(buf), &notify) == -FASTIX_EIO);
    CHECK(ring_write(r, buf, 8, false, &notify) == -FASTIX_EIO);
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 5); /* tail ahead of head: wraps to a huge distance */
    CHECK(ring_read(r, buf, sizeof(buf), &notify) == -FASTIX_EIO);
    CHECK(ring_write(r, buf, 8, false, &notify) == -FASTIX_EIO);
    atomic_store(&r->tail, 0);
    atomic_store(&r->closed_write, 1);
    atomic_store(&r->head, 999);
    CHECK(ring_read(r, buf, sizeof(buf), &notify) == -FASTIX_EIO);
}

static uint32_t rng_below(uint64_t *rng, uint32_t bound)
{
    return (uint32_t)(rng_next(rng) % bound);
}

static void run_seed(uint32_t seed)
{
    static const uint32_t caps[] = { 16, 64, 256, 1024, 4096 };
    uint64_t rng = seed;
    uint32_t cap = caps[rng_below(&rng, (uint32_t)(sizeof(caps) / sizeof(caps[0])))];

    uint8_t *mem = calloc(1, FASTIX_RING_DATA_OFFSET + cap);
    struct fastix_ring_hdr *r = (struct fastix_ring_hdr *)(void *)mem;
    uint32_t op = 0;
    CHECK(mem != NULL);
    CHECK(ring_init(r, cap, 1, 0) == 0);

    struct model m = { .cap = cap };
    uint8_t wbuf[MAX_CAP], rbuf[MAX_CAP];
    bool c_wait = false, p_wait = false;

    for (op = 0; op < OPS; op++) {
        uint32_t action = rng_below(&rng, 100);
        if (action < 45) {
            /* Write */
            uint32_t len = rng_below(&rng, cap + 32);
            if (len > MAX_CAP) {
                len = MAX_CAP;
            }
            bool atomic_all = (rng_below(&rng, 3) == 0) && len <= cap;
            for (uint32_t i = 0; i < len; i++) {
                wbuf[i] = (uint8_t)rng_next(&rng);
            }
            bool notify = false;
            int64_t got = ring_write(r, wbuf, len, atomic_all, &notify);

            if (m.closed_r) {
                CHECK(got == -FASTIX_EPIPE);
                CHECK(!notify);
            } else {
                uint32_t avail = model_free(&m);
                uint32_t expect = 0;
                if (len > 0 && avail > 0 && (!atomic_all || avail >= len)) {
                    expect = len < avail ? len : avail;
                }
                CHECK(got == (int64_t)expect);
                CHECK(notify == (expect > 0 && c_wait));
                for (uint32_t i = 0; i < expect; i++) {
                    m.buf[(m.head + i) % m.cap] = wbuf[i];
                }
                m.head += expect;
                if (notify) {
                    ring_clear_consumer_wait(r);
                    c_wait = false;
                }
            }
        } else if (action < 90) {
            /* Read */
            uint32_t len = rng_below(&rng, cap + 32);
            if (len > MAX_CAP) {
                len = MAX_CAP;
            }
            bool notify = false;
            int64_t got = ring_read(r, rbuf, len, &notify);

            if (len == 0) {
                CHECK(got == 0);
                CHECK(!notify);
            } else {
                uint32_t avail = model_used(&m);
                if (avail == 0) {
                    if (m.closed_w) {
                        CHECK(got == 0);
                    } else {
                        CHECK(got == -FASTIX_EAGAIN);
                    }
                    CHECK(!notify);
                } else {
                    uint32_t expect = len < avail ? len : avail;
                    CHECK(got == (int64_t)expect);
                    CHECK(notify == p_wait);
                    for (uint32_t i = 0; i < expect; i++) {
                        CHECK(rbuf[i] == m.buf[(m.tail + i) % m.cap]);
                    }
                    m.tail += expect;
                    if (notify) {
                        ring_clear_producer_wait(r);
                        p_wait = false;
                    }
                }
            }
        } else if (action < 94) {
            /* Arm consumer wait */
            bool armed = ring_arm_consumer_wait(r);
            bool expect = (model_used(&m) == 0 && !m.closed_w);
            CHECK(armed == expect);
            c_wait = armed;
        } else if (action < 98) {
            /* Arm producer wait */
            uint32_t need = 1u + rng_below(&rng, cap);
            bool armed = ring_arm_producer_wait(r, need);
            bool expect = (model_free(&m) < need && !m.closed_r);
            CHECK(armed == expect);
            p_wait = armed;
        } else if (action == 98 && !m.closed_w) {
            /* Close write end: remaining buffered bytes must still drain before EOF. */
            bool notify = false;
            ring_close_write(r, &notify);
            CHECK(notify == c_wait);
            m.closed_w = true;
            c_wait = false;
            ring_clear_consumer_wait(r);
        } else if (action == 99 && !m.closed_r) {
            /* Close read end: future writes must fail with -EPIPE. */
            bool notify = false;
            ring_close_read(r, &notify);
            CHECK(notify == p_wait);
            m.closed_r = true;
            p_wait = false;
            ring_clear_producer_wait(r);
        }

        CHECK(ring_used(r) == model_used(&m));
        CHECK(ring_free(r) == model_free(&m));
    }

    /* Final drain check: close write end and drain every remaining byte in order. */
    if (!m.closed_w) {
        bool notify = false;
        ring_close_write(r, &notify);
        m.closed_w = true;
    }
    while (model_used(&m) > 0) {
        uint8_t byte = 0;
        bool notify = false;
        int64_t got = ring_read(r, &byte, 1, &notify);
        CHECK(got == 1);
        CHECK(byte == m.buf[m.tail % m.cap]);
        m.tail++;
    }
    uint8_t eof_byte = 0;
    CHECK(ring_read(r, &eof_byte, 1, NULL) == 0);
    free(mem);
}

int main(void)
{
    test_init_errors();
    test_corrupt_counters();
    for (uint32_t seed = 1; seed <= SEEDS; seed++) {
        run_seed(seed);
    }
    printf("test_ring: %d seeds OK\n", SEEDS);
    return 0;
}
