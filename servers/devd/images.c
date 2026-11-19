/*
 * servers/devd/images.c - table of stripped driver ELFs embedded in devd.
 * Replaced by vfsd/cpiofs loading in Step 4D.
 */
#include "devd_internal.h"

extern const uint8_t fastix_drv_uart_pl011_start[], fastix_drv_uart_pl011_end[];
extern const uint8_t fastix_drv_rtc_pl031_start[], fastix_drv_rtc_pl031_end[];
extern const uint8_t fastix_drv_virtio_rng_start[], fastix_drv_virtio_rng_end[];
extern const uint8_t fastix_drv_virtio_blk_start[], fastix_drv_virtio_blk_end[];
extern const uint8_t fastix_drv_fault_test_start[], fastix_drv_fault_test_end[];

static const struct drv_image drv_images[FASTIX_DRV_COUNT] = {
    [FASTIX_DRV_UART]  = { "uart_pl011", fastix_drv_uart_pl011_start, fastix_drv_uart_pl011_end },
    [FASTIX_DRV_RTC]   = { "rtc_pl031",  fastix_drv_rtc_pl031_start,  fastix_drv_rtc_pl031_end  },
    [FASTIX_DRV_RNG]   = { "virtio_rng", fastix_drv_virtio_rng_start, fastix_drv_virtio_rng_end },
    [FASTIX_DRV_BLK]   = { "virtio_blk", fastix_drv_virtio_blk_start, fastix_drv_virtio_blk_end },
    [FASTIX_DRV_FAULT] = { "fault_test", fastix_drv_fault_test_start, fastix_drv_fault_test_end },
};

const struct drv_image *drv_image_get(uint32_t kind)
{
    if (kind >= FASTIX_DRV_COUNT) {
        return NULL;
    }
    return &drv_images[kind];
}
