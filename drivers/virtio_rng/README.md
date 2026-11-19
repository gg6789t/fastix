# virtio_rng

Isolated driver for the VirtIO entropy device (random bytes from the host). It owns only its
VirtIO-MMIO slot, one DMA page and its interrupt.

- **Operations:** `RNG_OP_READ` returns 1..48 random bytes in message registers. The length the
  device reports is clamped to what was asked for.
- **State:** one virtqueue (`libs/libdriver/virtio.c`), one request at a time; IPC rank 20.
- **Users (planned):** `/dev/random`, `getrandom()` and `AT_RANDOM` seeding (Phase 4D/5).
