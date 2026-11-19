/*
 * libs/libdriver/dma.c - DMA physical address query and cache maintenance.
 */
#include <arch/mmio.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <libdriver/dma.h>

int drv_dma_init(struct drv_dma_buf *buf, seL4_CPtr frame, void *vaddr, size_t size)
{
    if (buf == NULL || vaddr == NULL || size == 0 || size > FASTIX_PAGE_SIZE) {
        return -FASTIX_EINVAL;
    }
    seL4_ARM_Page_GetAddress_t res = seL4_ARM_Page_GetAddress(frame);
    if (res.error != seL4_NoError) {
        return -FASTIX_EINVAL;
    }
    buf->vaddr = vaddr;
    buf->paddr = res.paddr;
    buf->size = size;
    buf->frame = frame;
    return 0;
}

int drv_dma_to_device(const struct drv_dma_buf *buf, size_t off, size_t len)
{
    if (buf == NULL || len == 0 || off > buf->size || len > buf->size - off) {
        return -FASTIX_EINVAL;
    }
    seL4_Error err = seL4_ARM_Page_Clean_Data(buf->frame, off, off + len);
    if (err != seL4_NoError) {
        return -FASTIX_EINVAL;
    }
    arch_mb();
    return 0;
}

int drv_dma_from_device(const struct drv_dma_buf *buf, size_t off, size_t len)
{
    if (buf == NULL || len == 0 || off > buf->size || len > buf->size - off) {
        return -FASTIX_EINVAL;
    }
    seL4_Error err = seL4_ARM_Page_Invalidate_Data(buf->frame, off, off + len);
    if (err != seL4_NoError) {
        return -FASTIX_EINVAL;
    }
    arch_mb();
    return 0;
}
