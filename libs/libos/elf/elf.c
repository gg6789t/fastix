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
_Static_assert(sizeof(struct elf64_phdr) == ELF64_PHDR_SIZE, "ELF64 program header size");

#define ELF_CLASS64     2
#define ELF_DATA2LSB    1
#define ELF_VERSION     1
#define ELF_PT_LOAD     1

int elf_open(struct elf_image *img, const void *data, size_t size, uint16_t machine)
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
    if ((eh.type != ELF_ET_EXEC && eh.type != ELF_ET_DYN) || eh.machine != machine ||
        eh.phentsize != sizeof(struct elf64_phdr) || eh.phnum == 0) {
        return -FASTIX_ENOEXEC;
    }
    /* phnum <= 65535, so phnum * 56 cannot overflow. */
    if (eh.phoff > size || (uint64_t)eh.phnum * sizeof(struct elf64_phdr) > size - eh.phoff) {
        return -FASTIX_ENOEXEC;
    }
    img->data = data;
    img->size = size;
    img->type = eh.type;
    img->entry = eh.entry;
    img->phoff = eh.phoff;
    img->phnum = eh.phnum;
    img->bias = 0;
    img->lo = 0;
    img->hi = 0; /* nothing is loadable until elf_set_load */
    return 0;
}

int elf_set_load(struct elf_image *img, uint64_t bias, uint64_t lo, uint64_t hi)
{
    if ((img->type == ELF_ET_EXEC && bias != 0) || lo >= hi) {
        return -FASTIX_ENOEXEC;
    }
    uint64_t entry = img->entry + bias;
    if (entry < img->entry || entry < lo || entry >= hi) {
        return -FASTIX_ENOEXEC;
    }
    img->entry = entry;
    img->bias = bias;
    img->lo = lo;
    img->hi = hi;
    return 0;
}

/* Reads program header `index` (the caller checked index < phnum). */
static struct elf64_phdr read_phdr(const struct elf_image *img, unsigned index)
{
    struct elf64_phdr ph;
    memcpy(&ph, img->data + img->phoff + (uint64_t)index * sizeof(ph), sizeof(ph));
    return ph;
}

uint64_t elf_phdr_addr(const struct elf_image *img)
{
    uint64_t table = (uint64_t)img->phnum * sizeof(struct elf64_phdr);

    for (unsigned i = 0; i < img->phnum; i++) {
        struct elf_segment seg;
        if (elf_segment(img, i, &seg) != 1) {
            continue;
        }
        if (img->phoff >= seg.offset && img->phoff - seg.offset <= seg.filesz &&
            table <= seg.filesz - (img->phoff - seg.offset)) {
            return seg.vaddr + (img->phoff - seg.offset);
        }
    }
    return 0;
}

int elf_segment(const struct elf_image *img, unsigned index, struct elf_segment *seg)
{
    if (index >= img->phnum) {
        return -FASTIX_ENOEXEC;
    }
    struct elf64_phdr ph = read_phdr(img, index);
    if (ph.type != ELF_PT_LOAD || ph.memsz == 0) {
        return 0;
    }
    if (ph.filesz > ph.memsz || ph.offset > img->size || ph.filesz > img->size - ph.offset) {
        return -FASTIX_ENOEXEC;
    }
    uint64_t vaddr = ph.vaddr + img->bias;
    if (vaddr < ph.vaddr || vaddr < img->lo || vaddr >= img->hi || ph.memsz > img->hi - vaddr) {
        return -FASTIX_ENOEXEC;
    }
    if ((ph.flags & ELF_PF_W) && (ph.flags & ELF_PF_X)) {
        return -FASTIX_ENOEXEC; /* W^X */
    }
    seg->vaddr = vaddr;
    seg->memsz = ph.memsz;
    seg->offset = ph.offset;
    seg->filesz = ph.filesz;
    seg->flags = ph.flags & (ELF_PF_R | ELF_PF_W | ELF_PF_X);
    return 1;
}
