# virtio_blk

Isolated driver for the VirtIO block device (the virtual disk). It owns only its VirtIO-MMIO slot,
two DMA pages and its interrupt.

- **Operations:** `BLK_OP_INFO` (capacity in sectors, sector size), `BLK_OP_READ` and
  `BLK_OP_WRITE` (one 512-byte sector, carried in message registers). Sector numbers past the end
  of the disk are rejected with `EINVAL`.
- **State:** one virtqueue, one request at a time, synchronous; IPC rank 20.
- **Known limit:** one sector per message is slow. Before ext2 (Phase 4F) the data path must move
  to DMA directly into `vfsd`'s page-cache frames (AGENTS.md 6.3 D).
