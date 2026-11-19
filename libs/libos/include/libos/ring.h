/*
 * libos/ring.h - lock-free SPSC shared-memory ring buffer operations
 * over struct fastix_ring_hdr (AGENTS.md 5.6). Pure C17, host-testable.
 */
#ifndef LIBOS_RING_H
#define LIBOS_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <fastix/ring.h>
#include <libos/compiler.h>

/* Initializes a ring header whose data buffer starts immediately after it.
 * `capacity` must be a power of two >= 2. Returns 0 or -FASTIX_EINVAL. */
LIBOS_MUST_CHECK int ring_init(struct fastix_ring_hdr *r, uint32_t capacity, uint32_t elem_size,
                               uint32_t flags);

/* Current readable bytes and writable free bytes (acquire snapshot). */
uint32_t ring_used(const struct fastix_ring_hdr *r);
uint32_t ring_free(const struct fastix_ring_hdr *r);

/*
 * Writes up to `len` bytes from `src`.
 * If `atomic_all` is true and free space < len, writes 0 bytes (POSIX PIPE_BUF atomicity).
 * Returns bytes written (>= 0), or -FASTIX_EPIPE if consumer closed the read end.
 * Sets *need_notify = true iff bytes were written and consumer_waiting != 0.
 */
int64_t ring_write(struct fastix_ring_hdr *r, const void *src, size_t len, bool atomic_all,
                   bool *need_notify);

/*
 * Reads up to `len` bytes into `dst`.
 * Returns bytes read (> 0), 0 on EOF (empty and closed_write != 0),
 * or -FASTIX_EAGAIN if empty and producer is still open.
 * Sets *need_notify = true iff bytes were read and producer_waiting != 0.
 */
int64_t ring_read(struct fastix_ring_hdr *r, void *dst, size_t len, bool *need_notify);

/* Marks consumer waiting (seq_cst) and returns true if ring is still empty and open. */
bool ring_arm_consumer_wait(struct fastix_ring_hdr *r);
void ring_clear_consumer_wait(struct fastix_ring_hdr *r);

/* Marks producer waiting (seq_cst) and returns true if ring still has < need free bytes and read end open. */
bool ring_arm_producer_wait(struct fastix_ring_hdr *r, size_t need);
void ring_clear_producer_wait(struct fastix_ring_hdr *r);

/* Marks write or read end closed and sets *need_notify if peer is waiting. */
void ring_close_write(struct fastix_ring_hdr *r, bool *need_notify);
void ring_close_read(struct fastix_ring_hdr *r, bool *need_notify);

#endif /* LIBOS_RING_H */
