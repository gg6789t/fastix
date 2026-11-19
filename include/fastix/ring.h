/*
 * fastix/ring.h - lock-free shared-memory SPSC ring wire format (AGENTS.md 5.6).
 *
 * Cache-line isolated layout (64-byte lines) so producer and consumer never
 * false-share cache lines:
 *   Line 0 (  0.. 63): immutable metadata (magic, capacity, mask, flags, elem_size)
 *   Line 1 ( 64..127): producer-owned (head, producer_waiting, closed_write)
 *   Line 2 (128..191): consumer-owned (tail, consumer_waiting, closed_read)
 *   Line 3 (192..255): reserved padding
 *   Offset  256..    : power-of-two byte buffer of `capacity` bytes
 */
#ifndef FASTIX_RING_H
#define FASTIX_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define FASTIX_RING_MAGIC       0x46585247u /* "FXRG" */
#define FASTIX_RING_DATA_OFFSET 256u

struct fastix_ring_hdr {
    /* Cache line 0: immutable metadata */
    uint32_t magic;
    uint32_t capacity;      /* power of two >= 2 */
    uint32_t capacity_mask; /* capacity - 1 */
    uint32_t flags;
    uint32_t elem_size;
    uint32_t reserved0[11];

    /* Cache line 1: producer-owned */
    _Atomic uint64_t head;
    _Atomic uint32_t producer_waiting;
    _Atomic uint32_t closed_write;
    uint8_t pad1[48];

    /* Cache line 2: consumer-owned */
    _Atomic uint64_t tail;
    _Atomic uint32_t consumer_waiting;
    _Atomic uint32_t closed_read;
    uint8_t pad2[48];

    /* Cache line 3: reserved */
    uint8_t pad3[64];
};

_Static_assert(offsetof(struct fastix_ring_hdr, head) == 64, "ring head must start on line 1");
_Static_assert(offsetof(struct fastix_ring_hdr, tail) == 128, "ring tail must start on line 2");
_Static_assert(sizeof(struct fastix_ring_hdr) == FASTIX_RING_DATA_OFFSET,
               "ring header must be 256 bytes");

#endif /* FASTIX_RING_H */
