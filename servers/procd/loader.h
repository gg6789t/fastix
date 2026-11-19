/*
 * loader.h - loads a static ELF64 executable (fixed-address or PIE) into an
 * empty address space and builds the System V initial stack
 * (argc, argv, envp, auxv) that libc's _start expects (AGENTS.md 5.9).
 */
#ifndef PROCD_LOADER_H
#define PROCD_LOADER_H

#include <stddef.h>
#include <stdint.h>

#include <libos/compiler.h>

#include "vmspace.h"

struct load_args {
    const void *elf;          /* ELF image in procd memory */
    size_t elf_size;
    const char *const *argv;  /* NULL-terminated */
    const char *const *envp;  /* NULL-terminated */
    uint64_t random_seed;     /* source for the 16 AT_RANDOM bytes */
};

/* Returns 0 with the entry point and initial stack pointer, or -errno
 * (-FASTIX_ENOEXEC bad image, -FASTIX_E2BIG arguments too large). */
LIBOS_MUST_CHECK int loader_load(struct vmspace *vs, const struct load_args *a, uint64_t *entry,
                                 uint64_t *sp);

#endif /* PROCD_LOADER_H */
