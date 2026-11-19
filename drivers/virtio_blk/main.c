/*
 * drivers/virtio_blk/main.c - isolated VirtIO-MMIO block device driver.
 * Owns only its VirtIO-MMIO frame, IRQ handler cap, and 2 DMA pages
 * (DMA 0 = split virtqueue + request header/status, DMA 1 = 512 B sector buffer).
 */
#include <stdbool.h>
#include <stdint.h>

#include <fastix/config.h>
#include <fastix/dev_proto.h>
#include <fastix/errno.h>
#include <libdriver/mmio.h>
#include <libdriver/virtio.h>
#include <libos/mem.h>
#include <sel4/sel4.h>

/* VirtIO block request types and status codes (VirtIO 1.2 Section 5.2.6). */
#define VIRTIO_BLK_T_IN     0U
#define VIRTIO_BLK_T_OUT    1U
#define VIRTIO_BLK_S_OK     0U

#define SECTOR_WORDS        (FASTIX_BLK_SECTOR_SIZE / sizeof(uint64_t))
#define REQ_HDR_OFF         VIRTIO_VQ_SCRATCH_OFF
#define REQ_STATUS_OFF      (VIRTIO_VQ_SCRATCH_OFF + 64U)

struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

static struct virtio_dev vdev;
static struct drv_dma_buf data_dma;
static uint64_t capacity_sectors;
static int init_err;

static int blk_io(uint64_t sector, bool is_write)
{
    if (init_err != 0) {
        return init_err;
    }
    if (sector >= capacity_sectors) {
        return -FASTIX_EINVAL;
    }

    uint8_t *qbase = vdev.qdma.vaddr;
    struct virtio_blk_req_hdr *hdr = (struct virtio_blk_req_hdr *)(void *)(qbase + REQ_HDR_OFF);
    uint8_t *status = qbase + REQ_STATUS_OFF;

    hdr->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    hdr->reserved = 0;
    hdr->sector = sector;
    *status = 0xFFU;

    int err = drv_dma_to_device(&vdev.qdma, REQ_HDR_OFF, 128U);
    if (err != 0) {
        return err;
    }
    err = drv_dma_to_device(&data_dma, 0, FASTIX_BLK_SECTOR_SIZE);
    if (err != 0) {
        return err;
    }

    struct virtio_seg segs[3] = {
        { .paddr = vdev.qdma.paddr + REQ_HDR_OFF, .len = sizeof(*hdr), .dev_write = false },
        { .paddr = data_dma.paddr, .len = FASTIX_BLK_SECTOR_SIZE, .dev_write = !is_write },
        { .paddr = vdev.qdma.paddr + REQ_STATUS_OFF, .len = 1U, .dev_write = true },
    };

    err = virtio_submit_sync(&vdev, segs, 3, NULL);
    if (err != 0) {
        return err;
    }
    err = drv_dma_from_device(&vdev.qdma, REQ_STATUS_OFF, 64U);
    if (err != 0) {
        return err;
    }
    if (!is_write) {
        err = drv_dma_from_device(&data_dma, 0, FASTIX_BLK_SECTOR_SIZE);
        if (err != 0) {
            return err;
        }
    }
    return *status == VIRTIO_BLK_S_OK ? 0 : -FASTIX_EIO;
}

static seL4_MessageInfo_t handle_req(seL4_MessageInfo_t req)
{
    seL4_Word op = seL4_MessageInfo_get_label(req);
    seL4_Word n = seL4_MessageInfo_get_length(req);

    if (op == BLK_OP_INFO) {
        if (init_err != 0) {
            seL4_SetMR(0, (seL4_Word)(int64_t)init_err);
            return seL4_MessageInfo_new(0, 0, 0, 1);
        }
        seL4_SetMR(0, 0);
        seL4_SetMR(1, capacity_sectors);
        seL4_SetMR(2, FASTIX_BLK_SECTOR_SIZE);
        return seL4_MessageInfo_new(0, 0, 0, 3);
    }

    if (op == BLK_OP_READ) {
        if (n < 1U) {
            seL4_SetMR(0, (seL4_Word)(int64_t)-FASTIX_EINVAL);
            return seL4_MessageInfo_new(0, 0, 0, 1);
        }
        uint64_t sector = seL4_GetMR(0);
        int err = blk_io(sector, false);
        if (err != 0) {
            seL4_SetMR(0, (seL4_Word)(int64_t)err);
            return seL4_MessageInfo_new(0, 0, 0, 1);
        }
        const uint8_t *buf = data_dma.vaddr;
        seL4_SetMR(0, 0);
        for (size_t i = 0; i < SECTOR_WORDS; i++) {
            uint64_t w = 0;
            memcpy(&w, buf + i * sizeof(uint64_t), sizeof(uint64_t));
            seL4_SetMR(1U + i, w);
        }
        return seL4_MessageInfo_new(0, 0, 0, 1U + SECTOR_WORDS);
    }

    if (op == BLK_OP_WRITE) {
        if (init_err != 0) {
            seL4_SetMR(0, (seL4_Word)(int64_t)init_err);
            return seL4_MessageInfo_new(0, 0, 0, 1);
        }
        if (n < 1U + SECTOR_WORDS) {
            seL4_SetMR(0, (seL4_Word)(int64_t)-FASTIX_EINVAL);
            return seL4_MessageInfo_new(0, 0, 0, 1);
        }
        uint64_t sector = seL4_GetMR(0);
        uint8_t *buf = data_dma.vaddr;
        for (size_t i = 0; i < SECTOR_WORDS; i++) {
            uint64_t w = seL4_GetMR(1U + i);
            memcpy(buf + i * sizeof(uint64_t), &w, sizeof(uint64_t));
        }
        int err = blk_io(sector, true);
        seL4_SetMR(0, (seL4_Word)(int64_t)err);
        return seL4_MessageInfo_new(0, 0, 0, 1);
    }

    seL4_SetMR(0, (seL4_Word)(int64_t)-FASTIX_ENOSYS);
    return seL4_MessageInfo_new(0, 0, 0, 1);
}

_Noreturn void drv_main(const struct drv_bootinfo *bi)
{
    uintptr_t mmio = (uintptr_t)DRV_VADDR_MMIO + bi->mmio_offset;
    void *dma0 = (void *)(uintptr_t)DRV_VADDR_DMA_0;
    void *dma1 = (void *)(uintptr_t)DRV_VADDR_DMA_1;

    init_err = virtio_init(&vdev, mmio, VIRTIO_DEV_BLK,
                           DRV_CAP_DMA_FRAME_0, dma0,
                           DRV_CAP_IRQ_HANDLER, DRV_CAP_IRQ_NTFN);
    if (init_err == 0) {
        init_err = drv_dma_init(&data_dma, DRV_CAP_DMA_FRAME_1, dma1, FASTIX_PAGE_SIZE);
    }
    if (init_err == 0) {
        uint32_t cap_lo = mmio_read32(mmio + VIRTIO_MMIO_CONFIG);
        uint32_t cap_hi = mmio_read32(mmio + VIRTIO_MMIO_CONFIG + 4U);
        capacity_sectors = ((uint64_t)cap_hi << 32) | cap_lo;
    }

    seL4_Word badge = 0;
    seL4_MessageInfo_t msg = seL4_Recv(DRV_CAP_SERVICE_EP, &badge, DRV_CAP_REPLY);
    for (;;) {
        seL4_MessageInfo_t reply = handle_req(msg);
        msg = seL4_ReplyRecv(DRV_CAP_SERVICE_EP, reply, &badge, DRV_CAP_REPLY);
    }
}
