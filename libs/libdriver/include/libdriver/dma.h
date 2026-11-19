/*
 * libdriver/dma.h - DMA buffer descriptor and cache maintenance helpers.
 * A driver receives mapped RAM frame capabilities from devd, queries each
 * frame's physical address via seL4_ARM_Page_GetAddress, and cleans/invalidates
 * data cache lines around device transfers.
 */
#ifndef LIBDRIVER_DMA_H
#define LIBDRIVER_DMA_H

#include <stddef.h>
#include <stdint.h>

#include <sel4/sel4.h>

#include <libos/compiler.h>

struct drv_dma_buf {
    void *vaddr;
    uint64_t paddr;
    size_t size;
    seL4_CPtr frame;
};

/* Initializes *buf by querying frame's physical address from seL4.
 * size must be in [1, FASTIX_PAGE_SIZE]. Returns 0 or -FASTIX_EINVAL. */
LIBOS_MUST_CHECK int drv_dma_init(struct drv_dma_buf *buf, seL4_CPtr frame,
                                  void *vaddr, size_t size);

/* Flushes CPU writes in [off, off + len) out to RAM so the device sees them. */
LIBOS_MUST_CHECK int drv_dma_to_device(const struct drv_dma_buf *buf, size_t off, size_t len);

/* Invalidates CPU cache lines in [off, off + len) so the CPU sees device writes. */
LIBOS_MUST_CHECK int drv_dma_from_device(const struct drv_dma_buf *buf, size_t off, size_t len);

#endif /* LIBDRIVER_DMA_H */
