/*
 * drivers/virtio_rng/main.c - isolated VirtIO-MMIO entropy (RNG) driver.
 * Owns only its VirtIO-MMIO frame, IRQ handler cap, and 1 DMA page.
 */
#include <stdint.h>

#include <fastix/abi.h>
#include <fastix/dev_proto.h>
#include <fastix/errno.h>
#include <libdriver/virtio.h>
#include <libos/mem.h>
#include <sel4/sel4.h>

_Noreturn void drv_main(const struct drv_bootinfo *bi)
{
    uintptr_t mmio = (uintptr_t)DRV_VADDR_MMIO + bi->mmio_offset;
    void *dma0 = (void *)(uintptr_t)DRV_VADDR_DMA_0;
    struct virtio_dev vdev;

    int init_err = virtio_init(&vdev, mmio, VIRTIO_DEV_RNG,
                               DRV_CAP_DMA_FRAME_0, dma0,
                               DRV_CAP_IRQ_HANDLER, DRV_CAP_IRQ_NTFN);

    uint8_t *scratch = (uint8_t *)dma0 + VIRTIO_VQ_SCRATCH_OFF;
    uint64_t scratch_pa = vdev.qdma.paddr + VIRTIO_VQ_SCRATCH_OFF;

    seL4_Word badge = 0;
    seL4_MessageInfo_t msg = seL4_Recv(DRV_CAP_SERVICE_EP, &badge, DRV_CAP_REPLY);
    for (;;) {
        seL4_Word op = seL4_MessageInfo_get_label(msg);
        seL4_Word len_mrs = seL4_MessageInfo_get_length(msg);
        seL4_MessageInfo_t reply;

        if (op == RNG_OP_READ) {
            uint64_t want = len_mrs > 0 ? seL4_GetMR(0) : 0;
            if (init_err != 0) {
                seL4_SetMR(0, (seL4_Word)(int64_t)init_err);
                reply = seL4_MessageInfo_new(0, 0, 0, 1);
            } else if (want == 0 || want > FASTIX_IPC_INLINE_DATA_MAX) {
                seL4_SetMR(0, (seL4_Word)(int64_t)-FASTIX_EINVAL);
                reply = seL4_MessageInfo_new(0, 0, 0, 1);
            } else {
                memset(scratch, 0, FASTIX_IPC_INLINE_DATA_MAX);
                int err = drv_dma_to_device(&vdev.qdma, VIRTIO_VQ_SCRATCH_OFF,
                                            FASTIX_IPC_INLINE_DATA_MAX);
                uint32_t got = 0;
                if (err == 0) {
                    struct virtio_seg seg = {
                        .paddr = scratch_pa,
                        .len = (uint32_t)want,
                        .dev_write = true,
                    };
                    err = virtio_submit_sync(&vdev, &seg, 1, &got);
                }
                if (err == 0) {
                    err = drv_dma_from_device(&vdev.qdma, VIRTIO_VQ_SCRATCH_OFF,
                                              FASTIX_IPC_INLINE_DATA_MAX);
                }
                if (err != 0) {
                    seL4_SetMR(0, (seL4_Word)(int64_t)err);
                    reply = seL4_MessageInfo_new(0, 0, 0, 1);
                } else {
                    if (got > want) {
                        got = (uint32_t)want;
                    }
                    seL4_SetMR(0, got);
                    size_t nwords = (size_t)((got + 7U) / 8U);
                    for (size_t i = 0; i < nwords; i++) {
                        uint64_t w = 0;
                        memcpy(&w, scratch + i * 8U, 8U);
                        seL4_SetMR(1U + i, w);
                    }
                    reply = seL4_MessageInfo_new(0, 0, 0, 1U + nwords);
                }
            }
        } else {
            seL4_SetMR(0, (seL4_Word)(int64_t)-FASTIX_ENOSYS);
            reply = seL4_MessageInfo_new(0, 0, 0, 1);
        }
        msg = seL4_ReplyRecv(DRV_CAP_SERVICE_EP, reply, &badge, DRV_CAP_REPLY);
    }
}
