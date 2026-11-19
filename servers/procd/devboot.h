/*
 * devboot.h - bootstraps devd from procd and handles Rank 20 -> Rank 10
 * downward server-control calls (FASTIX_BADGE_SERVER).
 */
#ifndef PROCD_DEVBOOT_H
#define PROCD_DEVBOOT_H

#include <stdint.h>

#include <libos/compiler.h>
#include <sel4/sel4.h>

struct proc;

/* Spawns devd with DTB, Device Untypeds, IRQControl, SchedControl, ASIDPool,
 * and a 4 MB RAM untyped. Returns 0 or -errno. */
LIBOS_MUST_CHECK int devboot_start(const seL4_BootInfo *bi);

/* Installs devd's service endpoint into p's root CNode at FASTIX_CAP_DEVD_EP. */
LIBOS_MUST_CHECK int devboot_install_devd_ep(struct proc *p);

/* Installs a registered driver's service endpoint into p's root CNode at
 * FASTIX_CAP_DRV_FIRST + kind (used by FASTIX_TEST_GET_DRV_EP). */
LIBOS_MUST_CHECK int devboot_install_drv_ep(struct proc *p, uint32_t kind);

/* Handles a FASTIX_BADGE_SERVER IPC call in procd's main loop.
 * Writes reply words to the IPC buffer and returns *nret. */
void devboot_handle_server_msg(seL4_Word badge, seL4_MessageInfo_t info, unsigned *nret);

#endif /* PROCD_DEVBOOT_H */
