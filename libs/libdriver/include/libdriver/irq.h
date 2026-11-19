/*
 * libdriver/irq.h - thin seL4_IRQHandler wrapper for isolated drivers.
 */
#ifndef LIBDRIVER_IRQ_H
#define LIBDRIVER_IRQ_H

#include <sel4/sel4.h>

#include <libos/compiler.h>

struct drv_irq {
    seL4_CPtr handler;
    seL4_CPtr ntfn;
};

/* Binds handler to ntfn and issues the initial Ack to unmask the IRQ. */
LIBOS_MUST_CHECK int drv_irq_bind(struct drv_irq *irq, seL4_CPtr handler, seL4_CPtr ntfn);

/* Acknowledges a handled IRQ so seL4 unmasks the next interrupt. */
LIBOS_MUST_CHECK int drv_irq_ack(const struct drv_irq *irq);

#endif /* LIBDRIVER_IRQ_H */
