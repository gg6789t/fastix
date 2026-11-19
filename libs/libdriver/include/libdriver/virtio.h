/*
 * libdriver/virtio.h - VirtIO-MMIO v2 transport and split-virtqueue helper.
 * Shared by drivers/virtio_rng and drivers/virtio_blk.
 */
#ifndef LIBDRIVER_VIRTIO_H
#define LIBDRIVER_VIRTIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libdriver/dma.h>
#include <libdriver/irq.h>
#include <libos/compiler.h>

/* VirtIO-MMIO register offsets (VirtIO 1.2 Section 4.2.2). */
#define VIRTIO_MMIO_MAGIC             0x000U
#define VIRTIO_MMIO_VERSION           0x004U
#define VIRTIO_MMIO_DEVICE_ID         0x008U
#define VIRTIO_MMIO_VENDOR_ID         0x00CU
#define VIRTIO_MMIO_DEVICE_FEATURES   0x010U
#define VIRTIO_MMIO_DEVICE_FEAT_SEL   0x014U
#define VIRTIO_MMIO_DRIVER_FEATURES   0x020U
#define VIRTIO_MMIO_DRIVER_FEAT_SEL   0x024U
#define VIRTIO_MMIO_GUEST_PAGE_SIZE   0x028U /* legacy v1 only */
#define VIRTIO_MMIO_QUEUE_SEL         0x030U
#define VIRTIO_MMIO_QUEUE_NUM_MAX     0x034U
#define VIRTIO_MMIO_QUEUE_NUM         0x038U
#define VIRTIO_MMIO_QUEUE_ALIGN       0x03CU /* legacy v1 only */
#define VIRTIO_MMIO_QUEUE_PFN         0x040U /* legacy v1 only */
#define VIRTIO_MMIO_QUEUE_READY       0x044U /* modern v2 only */
#define VIRTIO_MMIO_QUEUE_NOTIFY      0x050U
#define VIRTIO_MMIO_INT_STATUS        0x060U
#define VIRTIO_MMIO_INT_ACK           0x064U
#define VIRTIO_MMIO_STATUS            0x070U
#define VIRTIO_MMIO_QUEUE_DESC_LOW    0x080U
#define VIRTIO_MMIO_QUEUE_DESC_HIGH   0x084U
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW   0x090U
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH  0x094U
#define VIRTIO_MMIO_QUEUE_USED_LOW    0x0A0U
#define VIRTIO_MMIO_QUEUE_USED_HIGH   0x0A4U
#define VIRTIO_MMIO_CONFIG            0x100U

#define VIRTIO_MAGIC_VALUE            0x74726976U /* "virt" */
#define VIRTIO_VERSION_LEGACY         1U
#define VIRTIO_VERSION_MODERN         2U

#define VIRTIO_DEV_BLK                2U
#define VIRTIO_DEV_RNG                4U

#define VIRTIO_STATUS_ACK             0x01U
#define VIRTIO_STATUS_DRIVER          0x02U
#define VIRTIO_STATUS_DRIVER_OK       0x04U
#define VIRTIO_STATUS_FEATURES_OK     0x08U

#define VIRTQ_DESC_F_NEXT             1U
#define VIRTQ_DESC_F_WRITE            2U

/* Split virtqueue layout inside one 4 KB DMA page (DRV_VADDR_DMA_0).
 * Size 8 fits QEMU virtio-rng (QueueNumMax = 8) and virtio-blk (max chain = 3).
 * With legacy QueueAlign = 256: desc=0x000 (128 B), avail=0x080 (22 B),
 * used=ALIGN_UP(0x096, 256) = 0x100 (70 B) — identical offsets in v1 and v2. */
#define VIRTIO_QSIZE                  8U
#define VIRTIO_VQ_DESC_OFF            0x000U
#define VIRTIO_VQ_AVAIL_OFF           0x080U
#define VIRTIO_VQ_USED_OFF            0x100U
#define VIRTIO_VQ_SCRATCH_OFF         0x400U /* 3 KB free scratch inside page 0 */

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_QSIZE];
    uint16_t used_event;
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VIRTIO_QSIZE];
    uint16_t avail_event;
};

struct virtio_seg {
    uint64_t paddr;
    uint32_t len;
    bool dev_write; /* true if device writes to this buffer */
};

struct virtio_dev {
    uintptr_t mmio;
    struct drv_dma_buf qdma;
    struct drv_irq irq;
    volatile struct virtq_desc *desc;
    volatile struct virtq_avail *avail;
    volatile struct virtq_used *used;
    uint16_t last_used;
};

/* Initializes a VirtIO-MMIO v2 device with one 16-entry split virtqueue (queue 0).
 * Returns 0 on success or negative -FASTIX_E* on failure. */
LIBOS_MUST_CHECK int virtio_init(struct virtio_dev *v, uintptr_t mmio, uint32_t expected_dev_id,
                                 seL4_CPtr qdma_frame, void *qdma_vaddr,
                                 seL4_CPtr irq_handler, seL4_CPtr irq_ntfn);

/* Submits a descriptor chain of 1..3 segments on queue 0 and waits for IRQ completion.
 * Writes bytes written by device to *used_len_out (if non-NULL). Returns 0 or -errno. */
LIBOS_MUST_CHECK int virtio_submit_sync(struct virtio_dev *v, const struct virtio_seg *segs,
                                        size_t nsegs, uint32_t *used_len_out);

#endif /* LIBDRIVER_VIRTIO_H */
