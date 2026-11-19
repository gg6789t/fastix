/* procd.h - procd entry point, called from arch crt0 with the seL4 BootInfo. */
#ifndef PROCD_H
#define PROCD_H

#include <sel4/sel4.h>

_Noreturn void procd_main(seL4_BootInfo *bi);

#endif /* PROCD_H */
