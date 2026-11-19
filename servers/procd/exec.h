/*
 * exec.h - execve (ELF + #! scripts up to depth 4) and posix_spawn_fast.
 */
#ifndef PROCD_EXEC_H
#define PROCD_EXEC_H

#include <stdint.h>

#include <libos/compiler.h>

#include "proc.h"

/*
 * Replaces caller's process image with the program/script at user pointer `path_u`.
 * On success returns 0 and caller is already restarted at the new entry point
 * (do NOT reply to the execve IPC). On error returns -errno and caller is unchanged.
 */
LIBOS_MUST_CHECK int proc_execve(struct thread *caller, uint64_t path_u, uint64_t argv_u,
                                 uint64_t envp_u);

/*
 * Spawns a new child directly into a fresh address space from struct fastix_spawn_args
 * at user pointer `args_u` (no fork/COW overhead). On 0 the caller must park in PARK_VFS:
 * it gets the child pid once vfsd has copied the FDs. Returns -errno on failure.
 */
LIBOS_MUST_CHECK int proc_posix_spawn(struct thread *caller, uint64_t args_u);

#endif /* PROCD_EXEC_H */
