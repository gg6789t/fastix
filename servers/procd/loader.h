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
    uint32_t uid;
    uint32_t euid;
    uint32_t gid;
    uint32_t egid;
    uint64_t vdso_ehdr;       /* AT_SYSINFO_EHDR (0 if none) */
};

/* Returns 0 with the entry point and initial stack pointer, or -errno
 * (-FASTIX_ENOEXEC bad image, -FASTIX_E2BIG arguments too large). */
LIBOS_MUST_CHECK int loader_load(struct vmspace *vs, const struct load_args *a, uint64_t *entry,
                                 uint64_t *sp);

/*
 * Checks if `data[0..size)` starts with "#!".
 * Returns 0 if not a shebang, 1 if parsed (`interp` and optional `arg` NUL-terminated;
 * `arg[0] == '\0'` when no optional argument), or -FASTIX_ENOEXEC if malformed.
 */
int loader_parse_shebang(const uint8_t *data, size_t size, char *interp, size_t interp_cap,
                         char *arg, size_t arg_cap);

#endif /* PROCD_LOADER_H */
