/* elf.c - bounds-checked reader for static ELF64 executables (see libos/elf.h). */
#include <libos/elf.h>
#include <libos/mem.h>

#include <fastix/errno.h>

/* On-disk layouts (ELF64, little-endian; Fastix targets are little-endian). */
struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

_Static_assert(sizeof(struct elf64_ehdr) == 64, "ELF64 header is 64 bytes");
_Static_assert(sizeof(struct elf64_phdr) == 56, "ELF64 program header is 56 bytes");

#define ELF_CLASS64     2
#define ELF_DATA2LSB    1
#define ELF_VERSION     1
#define ELF_ET_EXEC     2
#define ELF_PT_LOAD     1

int elf_open(struct elf_image *img, const void *data, size_t size, uint16_t machine,
             uint64_t lo, uint64_t hi)
{
    struct elf64_ehdr eh;

    if (size < sizeof(eh)) {
        return -FASTIX_ENOEXEC;
    }
    memcpy(&eh, data, sizeof(eh));
    if (eh.ident[0] != 0x7F || eh.ident[1] != 'E' || eh.ident[2] != 'L' || eh.ident[3] != 'F' ||
        eh.ident[4] != ELF_CLASS64 || eh.ident[5] != ELF_DATA2LSB ||
        eh.ident[6] != ELF_VERSION) {
        return -FASTIX_ENOEXEC;
    }
    if (eh.type != ELF_ET_EXEC || eh.machine != machine ||
        eh.phentsize != sizeof(struct elf64_phdr) || eh.phnum == 0) {
        return -FASTIX_ENOEXEC;
    }
    /* phnum <= 65535, so phnum * 56 cannot overflow. */
    if (eh.phoff > size || (uint64_t)eh.phnum * sizeof(struct elf64_phdr) > size - eh.phoff) {
        return -FASTIX_ENOEXEC;
    }
    if (lo >= hi || eh.entry < lo || eh.entry >= hi) {
        return -FASTIX_ENOEXEC;
    }
    img->data = data;
    img->size = size;
    img->entry = eh.entry;
    img->phoff = eh.phoff;
    img->phnum = eh.phnum;
    img->lo = lo;
    img->hi = hi;
    return 0;
}

int elf_segment(const struct elf_image *img, unsigned index, struct elf_segment *seg)
{
    struct elf64_phdr ph;

    if (index >= img->phnum) {
        return -FASTIX_ENOEXEC;
    }
    memcpy(&ph, img->data + img->phoff + (uint64_t)index * sizeof(ph), sizeof(ph));
    if (ph.type != ELF_PT_LOAD || ph.memsz == 0) {
        return 0;
    }
    if (ph.filesz > ph.memsz || ph.offset > img->size || ph.filesz > img->size - ph.offset) {
        return -FASTIX_ENOEXEC;
    }
    if (ph.vaddr < img->lo || ph.vaddr >= img->hi || ph.memsz > img->hi - ph.vaddr) {
        return -FASTIX_ENOEXEC;
    }
    if ((ph.flags & ELF_PF_W) && (ph.flags & ELF_PF_X)) {
        return -FASTIX_ENOEXEC; /* W^X */
    }
    seg->vaddr = ph.vaddr;
    seg->memsz = ph.memsz;
    seg->offset = ph.offset;
    seg->filesz = ph.filesz;
    seg->flags = ph.flags & (ELF_PF_R | ELF_PF_W | ELF_PF_X);
    return 1;
}
