/* arch/fault.h - decode AArch64 fault details delivered by seL4. */
#ifndef ARCH_FAULT_H
#define ARCH_FAULT_H

#include <stdbool.h>
#include <stdint.h>

#define ESR_WNR_BIT 6 /* data abort ISS: 1 = the access was a write */

/* True if a data abort (not an instruction fetch) was caused by a write. */
static inline bool arch_fault_is_write(uint64_t fsr)
{
    return ((fsr >> ESR_WNR_BIT) & 1) != 0;
}

#endif /* ARCH_FAULT_H */
