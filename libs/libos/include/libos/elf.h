/*
 * libos/elf.h - safe reader for static 64-bit ELF executables.
 * Never trusts the file: every offset, size and address is bounds-checked,
 * without overflow, before the caller sees it.
 */
#ifndef LIBOS_ELF_H
#define LIBOS_ELF_H

#include <stddef.h>
#include <stdint.h>

#include <libos/compiler.h>

#define ELF_PF_X 0x1u
#define ELF_PF_W 0x2u
#define ELF_PF_R 0x4u

struct elf_image {
    const uint8_t *data;
    size_t size;
    uint64_t entry;
    uint64_t phoff;
    uint16_t phnum;
    uint64_t lo, hi; /* allowed load address range [lo, hi) */
};

/* One loadable segment: memory [vaddr, vaddr + memsz), file bytes [offset, offset + filesz). */
struct elf_segment {
    uint64_t vaddr;
    uint64_t memsz;
    uint64_t offset;
    uint64_t filesz;
    uint32_t flags; /* ELF_PF_* */
};

/*
 * Checks the ELF header and program header table: 64-bit little-endian
 * ET_EXEC for `machine`, entry inside [lo, hi). Returns 0 or -FASTIX_ENOEXEC.
 */
LIBOS_MUST_CHECK int elf_open(struct elf_image *img, const void *data, size_t size,
                              uint16_t machine, uint64_t lo, uint64_t hi);

/*
 * Reads program header `index`. Returns 1 and fills *seg for a valid PT_LOAD
 * segment, 0 for a segment that is not loaded, or -FASTIX_ENOEXEC if invalid
 * (out of bounds, filesz > memsz, outside [lo, hi), or both writable and executable).
 */
LIBOS_MUST_CHECK int elf_segment(const struct elf_image *img, unsigned index,
                                 struct elf_segment *seg);

#endif /* LIBOS_ELF_H */
