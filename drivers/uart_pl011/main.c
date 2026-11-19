/*
 * drivers/uart_pl011/main.c - isolated ARM PrimeCell PL011 UART driver.
 * Owns only the PL011 MMIO frame and its SPI IRQ handler cap.
 */
#include <stdbool.h>
#include <stdint.h>

#include <fastix/abi.h>
#include <fastix/dev_proto.h>
#include <fastix/errno.h>
#include <libdriver/irq.h>
#include <libdriver/mmio.h>
#include <libos/mem.h>
#include <sel4/sel4.h>

/* PL011 register offsets (ARM DDI 0183). */
#define PL011_DR      0x000U
#define PL011_FR      0x018U
#define PL011_IMSC    0x038U
#define PL011_ICR     0x044U

#define PL011_FR_RXFE (1U << 4)
#define PL011_FR_TXFF (1U << 5)
#define PL011_INT_RX  (1U << 4)

#define RX_RING_CAP   256U

static uintptr_t uart_base;
static struct drv_irq uart_irq;
static uint8_t rx_buf[RX_RING_CAP];
static uint32_t rx_head;
static uint32_t rx_tail;
static uint64_t tx_total;
static uint64_t rx_irqs;

static void pl011_putc(uint8_t c)
{
    while ((mmio_read32(uart_base + PL011_FR) & PL011_FR_TXFF) != 0) {
    }
    mmio_write32(uart_base + PL011_DR, c);
    tx_total++;
}

static void pl011_drain_rx(void)
{
    while ((mmio_read32(uart_base + PL011_FR) & PL011_FR_RXFE) == 0) {
        uint8_t b = (uint8_t)(mmio_read32(uart_base + PL011_DR) & 0xFFU);
        uint32_t next = (rx_head + 1U) % RX_RING_CAP;
        if (next != rx_tail) {
            rx_buf[rx_head] = b;
            rx_head = next;
        }
    }
    mmio_write32(uart_base + PL011_ICR, PL011_INT_RX);
}

static seL4_MessageInfo_t handle_req(const struct drv_bootinfo *bi, seL4_MessageInfo_t req)
{
    seL4_Word op = seL4_MessageInfo_get_label(req);
    seL4_Word len_mrs = seL4_MessageInfo_get_length(req);

    if (op == UART_OP_WRITE) {
        uint64_t n = len_mrs > 0 ? seL4_GetMR(0) : 0;
        if (n > FASTIX_IPC_INLINE_DATA_MAX || (len_mrs - 1U) * 8U < n) {
            seL4_SetMR(0, (seL4_Word)(int64_t)-FASTIX_EINVAL);
            return seL4_MessageInfo_new(0, 0, 0, 1);
        }
        uint8_t tmp[FASTIX_IPC_INLINE_DATA_MAX];
        for (size_t i = 0; i < (n + 7U) / 8U; i++) {
            uint64_t w = seL4_GetMR(1U + i);
            memcpy(tmp + i * 8U, &w, 8U);
        }
        for (uint64_t i = 0; i < n; i++) {
            pl011_putc(tmp[i]);
        }
        seL4_SetMR(0, n);
        return seL4_MessageInfo_new(0, 0, 0, 1);
    }

    if (op == UART_OP_READ) {
        pl011_drain_rx();
        uint64_t want = len_mrs > 0 ? seL4_GetMR(0) : 0;
        if (want > FASTIX_IPC_INLINE_DATA_MAX) {
            want = FASTIX_IPC_INLINE_DATA_MAX;
        }
        uint8_t tmp[FASTIX_IPC_INLINE_DATA_MAX];
        memset(tmp, 0, sizeof(tmp));
        uint64_t got = 0;
        while (got < want && rx_tail != rx_head) {
            tmp[got++] = rx_buf[rx_tail];
            rx_tail = (rx_tail + 1U) % RX_RING_CAP;
        }
        seL4_SetMR(0, got);
        size_t nwords = (size_t)((got + 7U) / 8U);
        for (size_t i = 0; i < nwords; i++) {
            uint64_t w = 0;
            memcpy(&w, tmp + i * 8U, 8U);
            seL4_SetMR(1U + i, w);
        }
        return seL4_MessageInfo_new(0, 0, 0, 1U + nwords);
    }

    if (op == UART_OP_INFO) {
        seL4_SetMR(0, 0);
        seL4_SetMR(1, bi->mmio_paddr);
        seL4_SetMR(2, bi->irq);
        seL4_SetMR(3, tx_total);
        seL4_SetMR(4, rx_irqs);
        return seL4_MessageInfo_new(0, 0, 0, 5);
    }

    seL4_SetMR(0, (seL4_Word)(int64_t)-FASTIX_ENOSYS);
    return seL4_MessageInfo_new(0, 0, 0, 1);
}

_Noreturn void drv_main(const struct drv_bootinfo *bi)
{
    uart_base = (uintptr_t)DRV_VADDR_MMIO + bi->mmio_offset;

    if (bi->irq != 0 && drv_irq_bind(&uart_irq, DRV_CAP_IRQ_HANDLER, DRV_CAP_IRQ_NTFN) == 0) {
        mmio_write32(uart_base + PL011_IMSC, PL011_INT_RX);
    }

    seL4_Word badge = 0;
    seL4_MessageInfo_t msg = seL4_Recv(DRV_CAP_SERVICE_EP, &badge, DRV_CAP_REPLY);
    for (;;) {
        if (badge != 0) {
            /* Bound IRQ notification delivered while waiting on service_ep. */
            rx_irqs++;
            pl011_drain_rx();
            (void)drv_irq_ack(&uart_irq);
            msg = seL4_Recv(DRV_CAP_SERVICE_EP, &badge, DRV_CAP_REPLY);
            continue;
        }
        seL4_MessageInfo_t reply = handle_req(bi, msg);
        msg = seL4_ReplyRecv(DRV_CAP_SERVICE_EP, reply, &badge, DRV_CAP_REPLY);
    }
}
