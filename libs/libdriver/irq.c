/*
 * libs/libdriver/irq.c - seL4_IRQHandler bind/ack/clear helpers.
 */
#include <stddef.h>

#include <fastix/errno.h>
#include <libdriver/irq.h>

int drv_irq_bind(struct drv_irq *irq, seL4_CPtr handler, seL4_CPtr ntfn)
{
    if (irq == NULL || handler == seL4_CapNull || ntfn == seL4_CapNull) {
        return -FASTIX_EINVAL;
    }
    seL4_Error err = seL4_IRQHandler_SetNotification(handler, ntfn);
    if (err != seL4_NoError) {
        return -FASTIX_EINVAL;
    }
    err = seL4_IRQHandler_Ack(handler);
    if (err != seL4_NoError) {
        return -FASTIX_EINVAL;
    }
    irq->handler = handler;
    irq->ntfn = ntfn;
    return 0;
}

int drv_irq_ack(const struct drv_irq *irq)
{
    if (irq == NULL || irq->handler == seL4_CapNull) {
        return -FASTIX_EINVAL;
    }
    return seL4_IRQHandler_Ack(irq->handler) == seL4_NoError ? 0 : -FASTIX_EINVAL;
}
