/*
 * server.h - procd's main loop: receives syscalls and faults on one endpoint
 * and answers them. Calls that must wait are parked with their Reply object
 * so the loop never blocks (AGENTS.md 5.5).
 */
#ifndef PROCD_SERVER_H
#define PROCD_SERVER_H

#include <libos/compiler.h>
#include <sel4/sel4.h>

/* Creates the endpoint and the first Reply object. */
LIBOS_MUST_CHECK int server_init(void);

seL4_CPtr server_endpoint(void);

/* Serves forever; stops QEMU with PID 1's exit status when PID 1 ends. */
_Noreturn void server_run(void);

#endif /* PROCD_SERVER_H */
