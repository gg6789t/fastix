/*
 * libs/libdriver/virtio.c - VirtIO-MMIO v2 initialization and synchronous queue submission.
 */
#include <fastix/config.h>
#include <fastix/errno.h>
#include <libdriver/mmio.h>
#include <libdriver/virtio.h>
#include <libos/mem.h>

#define VIRTIO_MAX_CHAIN 3U

int virtio_init(struct virtio_dev *v, uintptr_t mmio, uint32_t expected_dev_id,
                seL4_CPtr qdma_frame, void *qdma_vaddr,
                seL4_CPtr irq_handler, seL4_CPtr irq_ntfn)
{
    if (v == NULL || mmio == 0 || qdma_vaddr == NULL) {
        return -FASTIX_EINVAL;
    }
    if (mmio_read32(mmio + VIRTIO_MMIO_MAGIC) != VIRTIO_MAGIC_VALUE ||
        mmio_read32(mmio + VIRTIO_MMIO_DEVICE_ID) != expected_dev_id) {
        return -FASTIX_ENODEV;
    }
    uint32_t ver = mmio_read32(mmio + VIRTIO_MMIO_VERSION);
    if (ver != VIRTIO_VERSION_LEGACY && ver != VIRTIO_VERSION_MODERN) {
        return -FASTIX_ENODEV;
    }

    int err = drv_dma_init(&v->qdma, qdma_frame, qdma_vaddr, FASTIX_PAGE_SIZE);
    if (err != 0) {
        return err;
    }
    err = drv_irq_bind(&v->irq, irq_handler, irq_ntfn);
    if (err != 0) {
        return err;
    }

    /* Reset device, then follow VirtIO 1.2 Section 3.1.1 / 4.2.4 initialization. */
    mmio_write32(mmio + VIRTIO_MMIO_STATUS, 0);
    while (mmio_read32(mmio + VIRTIO_MMIO_STATUS) != 0) {
    }
    mmio_write32(mmio + VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    uint32_t st = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER;
    mmio_write32(mmio + VIRTIO_MMIO_STATUS, st);

    if (ver == VIRTIO_VERSION_MODERN) {
        /* Accept only VIRTIO_F_VERSION_1 (bit 32 = bit 0 of feature word 1). */
        mmio_write32(mmio + VIRTIO_MMIO_DEVICE_FEAT_SEL, 1U);
        uint32_t dev_feat_hi = mmio_read32(mmio + VIRTIO_MMIO_DEVICE_FEATURES);
        if ((dev_feat_hi & 1U) == 0) {
            return -FASTIX_ENODEV;
        }
        mmio_write32(mmio + VIRTIO_MMIO_DRIVER_FEAT_SEL, 0U);
        mmio_write32(mmio + VIRTIO_MMIO_DRIVER_FEATURES, 0U);
        mmio_write32(mmio + VIRTIO_MMIO_DRIVER_FEAT_SEL, 1U);
        mmio_write32(mmio + VIRTIO_MMIO_DRIVER_FEATURES, 1U);

        st |= VIRTIO_STATUS_FEATURES_OK;
        mmio_write32(mmio + VIRTIO_MMIO_STATUS, st);
        if ((mmio_read32(mmio + VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) {
            return -FASTIX_EIO;
        }
    } else {
        /* Legacy v1: negotiate zero optional features and set guest page size. */
        mmio_write32(mmio + VIRTIO_MMIO_DRIVER_FEAT_SEL, 0U);
        mmio_write32(mmio + VIRTIO_MMIO_DRIVER_FEATURES, 0U);
        mmio_write32(mmio + VIRTIO_MMIO_GUEST_PAGE_SIZE, FASTIX_PAGE_SIZE);
    }

    /* Configure split virtqueue 0 inside qdma page (desc=0x000, avail=0x100, used=0x200). */
    mmio_write32(mmio + VIRTIO_MMIO_QUEUE_SEL, 0U);
    if (mmio_read32(mmio + VIRTIO_MMIO_QUEUE_NUM_MAX) < VIRTIO_QSIZE) {
        return -FASTIX_EIO;
    }
    memset(qdma_vaddr, 0, FASTIX_PAGE_SIZE);
    err = drv_dma_to_device(&v->qdma, 0, FASTIX_PAGE_SIZE);
    if (err != 0) {
        return err;
    }

    mmio_write32(mmio + VIRTIO_MMIO_QUEUE_NUM, VIRTIO_QSIZE);
    if (ver == VIRTIO_VERSION_MODERN) {
        uint64_t desc_pa = v->qdma.paddr + VIRTIO_VQ_DESC_OFF;
        uint64_t avail_pa = v->qdma.paddr + VIRTIO_VQ_AVAIL_OFF;
        uint64_t used_pa = v->qdma.paddr + VIRTIO_VQ_USED_OFF;

        mmio_write32(mmio + VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)desc_pa);
        mmio_write32(mmio + VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc_pa >> 32));
        mmio_write32(mmio + VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)avail_pa);
        mmio_write32(mmio + VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(avail_pa >> 32));
        mmio_write32(mmio + VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)used_pa);
        mmio_write32(mmio + VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(used_pa >> 32));
        mmio_write32(mmio + VIRTIO_MMIO_QUEUE_READY, 1U);
    } else {
        /* Legacy align = 256 places used ring at ALIGN_UP(256 + 38, 256) = 0x200. */
        mmio_write32(mmio + VIRTIO_MMIO_QUEUE_ALIGN, 256U);
        mmio_write32(mmio + VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(v->qdma.paddr >> FASTIX_PAGE_SHIFT));
    }

    mmio_write32(mmio + VIRTIO_MMIO_STATUS, st | VIRTIO_STATUS_DRIVER_OK);

    uint8_t *base = qdma_vaddr;
    v->mmio = mmio;
    v->desc = (volatile struct virtq_desc *)(void *)(base + VIRTIO_VQ_DESC_OFF);
    v->avail = (volatile struct virtq_avail *)(void *)(base + VIRTIO_VQ_AVAIL_OFF);
    v->used = (volatile struct virtq_used *)(void *)(base + VIRTIO_VQ_USED_OFF);
    v->last_used = 0;
    return 0;
}

int virtio_submit_sync(struct virtio_dev *v, const struct virtio_seg *segs,
                       size_t nsegs, uint32_t *used_len_out)
{
    if (v == NULL || segs == NULL || nsegs == 0 || nsegs > VIRTIO_MAX_CHAIN) {
        return -FASTIX_EINVAL;
    }

    for (size_t i = 0; i < nsegs; i++) {
        uint16_t flags = segs[i].dev_write ? VIRTQ_DESC_F_WRITE : 0U;
        if (i + 1 < nsegs) {
            flags = (uint16_t)(flags | VIRTQ_DESC_F_NEXT);
        }
        v->desc[i].addr = segs[i].paddr;
        v->desc[i].len = segs[i].len;
        v->desc[i].flags = flags;
        v->desc[i].next = (uint16_t)(i + 1);
    }

    uint16_t aidx = v->avail->idx;
    v->avail->ring[aidx % VIRTIO_QSIZE] = 0;
    arch_wmb();
    v->avail->idx = (uint16_t)(aidx + 1U);

    int err = drv_dma_to_device(&v->qdma, 0, VIRTIO_VQ_USED_OFF);
    if (err != 0) {
        return err;
    }
    mmio_write32(v->mmio + VIRTIO_MMIO_QUEUE_NOTIFY, 0U);

    for (;;) {
        seL4_Word badge = 0;
        seL4_Wait(v->irq.ntfn, &badge);

        uint32_t isr = mmio_read32(v->mmio + VIRTIO_MMIO_INT_STATUS);
        if (isr != 0) {
            mmio_write32(v->mmio + VIRTIO_MMIO_INT_ACK, isr);
        }
        err = drv_irq_ack(&v->irq);
        if (err != 0) {
            return err;
        }
        err = drv_dma_from_device(&v->qdma, VIRTIO_VQ_USED_OFF,
                                  VIRTIO_VQ_SCRATCH_OFF - VIRTIO_VQ_USED_OFF);
        if (err != 0) {
            return err;
        }
        arch_rmb();
        if (v->used->idx != v->last_used) {
            break;
        }
    }

    uint32_t used_len = v->used->ring[v->last_used % VIRTIO_QSIZE].len;
    v->last_used++;
    if (used_len_out != NULL) {
        *used_len_out = used_len;
    }
    return 0;
}
