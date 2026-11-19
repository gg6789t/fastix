/*
 * proc.h - creating and destroying user processes.
 * Phase 1: a process is one thread running a static ELF, with a 64-slot CNode
 * holding only its badged procd endpoint (cspace.h slot layout).
 */
#ifndef PROCD_PROC_H
#define PROCD_PROC_H

#include <stddef.h>
#include <stdint.h>

#include <libos/compiler.h>
#include <sel4/sel4.h>

#include "vm.h"

struct proc {
    uint32_t pid;
    uint16_t gen;          /* badge generation, so stale badges never match a new process */
    struct objlist objs;   /* every kernel object this process owns */
    seL4_CPtr vspace;
    seL4_CPtr cnode;
    seL4_CPtr tcb;
    seL4_CPtr sc;
    seL4_CPtr fault_ep;    /* badged fault endpoint copy in procd's CNode (0 if none) */
};

struct proc_spawn_args {
    uint32_t pid;
    uint16_t gen;
    const void *elf;       /* static ELF image in procd memory */
    size_t elf_size;
    seL4_CPtr endpoint;    /* procd's endpoint; minted with syscall/fault badges */
    seL4_CPtr sched_control;
    uint64_t arg0;         /* first argument register of the new thread */
};

/* Builds the process and starts its thread. On failure everything is freed. */
LIBOS_MUST_CHECK int proc_spawn(struct proc *p, const struct proc_spawn_args *args);

/* Stops the process and frees every kernel object and slot it held. */
void proc_destroy(struct proc *p);

#endif /* PROCD_PROC_H */
