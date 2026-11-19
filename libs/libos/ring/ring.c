/* ring.c - lock-free SPSC shared-memory ring buffer (see libos/ring.h). */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include <fastix/errno.h>
#include <libos/mem.h>
#include <libos/ring.h>

static uint8_t *ring_data(struct fastix_ring_hdr *r)
{
    return (uint8_t *)r + FASTIX_RING_DATA_OFFSET;
}

int ring_init(struct fastix_ring_hdr *r, uint32_t capacity, uint32_t elem_size, uint32_t flags)
{
    if (r == NULL || capacity < 2u || (capacity & (capacity - 1u)) != 0 || elem_size == 0) {
        return -FASTIX_EINVAL;
    }
    memset(r, 0, sizeof(*r));
    r->magic = FASTIX_RING_MAGIC;
    r->capacity = capacity;
    r->capacity_mask = capacity - 1u;
    r->flags = flags;
    r->elem_size = elem_size;
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&r->producer_waiting, 0, memory_order_relaxed);
    atomic_store_explicit(&r->consumer_waiting, 0, memory_order_relaxed);
    atomic_store_explicit(&r->closed_write, 0, memory_order_relaxed);
    atomic_store_explicit(&r->closed_read, 0, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    return 0;
}

uint32_t ring_used(const struct fastix_ring_hdr *r)
{
    /* Cast away const only because C11 atomic_load_explicit takes a non-const pointer. */
    struct fastix_ring_hdr *m = (struct fastix_ring_hdr *)(uintptr_t)r;
    uint64_t head = atomic_load_explicit(&m->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&m->tail, memory_order_acquire);
    uint64_t diff = head - tail;
    return diff > r->capacity ? r->capacity : (uint32_t)diff;
}

uint32_t ring_free(const struct fastix_ring_hdr *r)
{
    return r->capacity - ring_used(r);
}

int64_t ring_write(struct fastix_ring_hdr *r, const void *src, size_t len, bool atomic_all,
                   bool *need_notify)
{
    if (need_notify != NULL) {
        *need_notify = false;
    }
    if (atomic_load_explicit(&r->closed_read, memory_order_acquire) != 0) {
        return -FASTIX_EPIPE;
    }
    if (len == 0) {
        return 0;
    }
    uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    uint64_t used = head - tail;
    if (used > r->capacity) {
        return -FASTIX_EIO; /* corrupted counters (e.g. a misbehaving peer) */
    }
    uint64_t avail = r->capacity - used;

    if (avail == 0 || (atomic_all && avail < len)) {
        return 0;
    }
    size_t n = len < avail ? len : (size_t)avail;
    uint32_t idx = (uint32_t)(head & r->capacity_mask);
    uint32_t first = r->capacity - idx;
    if (first > n) {
        first = (uint32_t)n;
    }
    uint8_t *buf = ring_data(r);
    const uint8_t *in = src;
    memcpy(buf + idx, in, first);
    if (n > first) {
        memcpy(buf, in + first, n - first);
    }

    atomic_store_explicit(&r->head, head + n, memory_order_release);
    if (need_notify != NULL &&
        atomic_load_explicit(&r->consumer_waiting, memory_order_seq_cst) != 0) {
        *need_notify = true;
    }
    return (int64_t)n;
}

int64_t ring_read(struct fastix_ring_hdr *r, void *dst, size_t len, bool *need_notify)
{
    if (need_notify != NULL) {
        *need_notify = false;
    }
    if (len == 0) {
        return 0;
    }
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    uint64_t avail = head - tail;
    if (avail > r->capacity) {
        return -FASTIX_EIO; /* corrupted counters: never copy past the buffer */
    }
    if (avail == 0) {
        if (atomic_load_explicit(&r->closed_write, memory_order_acquire) != 0) {
            /* Re-check head after seeing closed_write to avoid missing final bytes. */
            head = atomic_load_explicit(&r->head, memory_order_acquire);
            avail = head - tail;
            if (avail == 0) {
                return 0; /* EOF */
            }
            if (avail > r->capacity) {
                return -FASTIX_EIO;
            }
        } else {
            return -FASTIX_EAGAIN;
        }
    }

    size_t n = len < avail ? len : (size_t)avail;
    uint32_t idx = (uint32_t)(tail & r->capacity_mask);
    uint32_t first = r->capacity - idx;
    if (first > n) {
        first = (uint32_t)n;
    }
    const uint8_t *buf = ring_data(r);
    uint8_t *out = dst;
    memcpy(out, buf + idx, first);
    if (n > first) {
        memcpy(out + first, buf, n - first);
    }

    atomic_store_explicit(&r->tail, tail + n, memory_order_release);
    if (need_notify != NULL &&
        atomic_load_explicit(&r->producer_waiting, memory_order_seq_cst) != 0) {
        *need_notify = true;
    }
    return (int64_t)n;
}

bool ring_arm_consumer_wait(struct fastix_ring_hdr *r)
{
    atomic_store_explicit(&r->consumer_waiting, 1, memory_order_seq_cst);
    uint64_t head = atomic_load_explicit(&r->head, memory_order_seq_cst);
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    if (head != tail || atomic_load_explicit(&r->closed_write, memory_order_acquire) != 0) {
        atomic_store_explicit(&r->consumer_waiting, 0, memory_order_relaxed);
        return false;
    }
    return true;
}

void ring_clear_consumer_wait(struct fastix_ring_hdr *r)
{
    atomic_store_explicit(&r->consumer_waiting, 0, memory_order_relaxed);
}

bool ring_arm_producer_wait(struct fastix_ring_hdr *r, size_t need)
{
    atomic_store_explicit(&r->producer_waiting, 1, memory_order_seq_cst);
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_seq_cst);
    uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint64_t avail = r->capacity - (head - tail);
    if (avail >= need || atomic_load_explicit(&r->closed_read, memory_order_acquire) != 0) {
        atomic_store_explicit(&r->producer_waiting, 0, memory_order_relaxed);
        return false;
    }
    return true;
}

void ring_clear_producer_wait(struct fastix_ring_hdr *r)
{
    atomic_store_explicit(&r->producer_waiting, 0, memory_order_relaxed);
}

void ring_close_write(struct fastix_ring_hdr *r, bool *need_notify)
{
    atomic_store_explicit(&r->closed_write, 1, memory_order_release);
    if (need_notify != NULL) {
        *need_notify = atomic_load_explicit(&r->consumer_waiting, memory_order_seq_cst) != 0;
    }
}

void ring_close_read(struct fastix_ring_hdr *r, bool *need_notify)
{
    atomic_store_explicit(&r->closed_read, 1, memory_order_release);
    if (need_notify != NULL) {
        *need_notify = atomic_load_explicit(&r->producer_waiting, memory_order_seq_cst) != 0;
    }
}
