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

#define ELF64_PHDR_SIZE 56 /* bytes per program header (AT_PHENT) */

#define ELF_ET_EXEC 2  /* fixed-address executable */
#define ELF_ET_DYN  3  /* position-independent executable (static PIE) */

struct elf_image {
    const uint8_t *data;
    size_t size;
    uint16_t type;   /* ELF_ET_EXEC or ELF_ET_DYN */
    uint64_t entry;  /* with bias applied, after elf_set_load */
    uint64_t phoff;
    uint16_t phnum;
    uint64_t bias;   /* added to every address (0 for ELF_ET_EXEC) */
    uint64_t lo, hi; /* allowed load address range [lo, hi), with bias applied */
};

/* One loadable segment: memory [vaddr, vaddr + memsz) (bias applied), file bytes [offset, offset + filesz). */
struct elf_segment {
    uint64_t vaddr;
    uint64_t memsz;
    uint64_t offset;
    uint64_t filesz;
    uint32_t flags; /* ELF_PF_* */
};

/*
 * Checks the ELF header and program header table: 64-bit little-endian
 * ET_EXEC or ET_DYN for `machine`. Returns 0 or -FASTIX_ENOEXEC.
 */
LIBOS_MUST_CHECK int elf_open(struct elf_image *img, const void *data, size_t size,
                              uint16_t machine);

/*
 * Sets where the image loads: every address gets `bias` added (must be 0 for
 * ET_EXEC) and must fall inside [lo, hi). Checks the biased entry point.
 * Returns 0 or -FASTIX_ENOEXEC.
 */
LIBOS_MUST_CHECK int elf_set_load(struct elf_image *img, uint64_t bias, uint64_t lo, uint64_t hi);

/* Address of the program headers in memory (for AT_PHDR), or 0 if no loaded segment holds them. */
uint64_t elf_phdr_addr(const struct elf_image *img);

/*
 * Reads program header `index`. Returns 1 and fills *seg for a valid PT_LOAD
 * segment (call elf_set_load first), 0 for a segment that is not loaded, or -FASTIX_ENOEXEC if invalid
 * (out of bounds, filesz > memsz, outside [lo, hi), or both writable and executable).
 */
LIBOS_MUST_CHECK int elf_segment(const struct elf_image *img, unsigned index,
                                 struct elf_segment *seg);

#endif /* LIBOS_ELF_H */
