/* loader.c - ELF loading and the System V initial stack (see loader.h). */
#include <arch/vm.h>
#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <fastix/mman.h>
#include <fastix/vspace.h>
#include <libos/elf.h>

#include "loader.h"

#define PAGE_MASK  ((uint64_t)FASTIX_PAGE_SIZE - 1)
#define NUM_AUXV   17 /* entries written by build_stack, including AT_NULL */
#define WBUF_WORDS 32

static uint32_t segment_prot(uint32_t flags)
{
    return ((flags & ELF_PF_R) ? FASTIX_PROT_READ : 0) |
           ((flags & ELF_PF_W) ? FASTIX_PROT_WRITE : 0) |
           ((flags & ELF_PF_X) ? FASTIX_PROT_EXEC : 0);
}

/* Maps every PT_LOAD segment and copies its file bytes; *brk = end of the highest segment. */
static int load_segments(struct vmspace *vs, const struct elf_image *img, uint64_t *brk)
{
    *brk = 0;
    for (unsigned i = 0; i < img->phnum; i++) {
        struct elf_segment seg;
        int r = elf_segment(img, i, &seg);
        if (r <= 0) {
            if (r < 0) {
                return r;
            }
            continue;
        }
        uint64_t start = seg.vaddr & ~PAGE_MASK;
        uint64_t end = (seg.vaddr + seg.memsz + PAGE_MASK) & ~PAGE_MASK; /* within [lo, hi) */
        uint64_t got;
        int err = vms_mmap(vs, start, end - start, segment_prot(seg.flags),
                           FASTIX_MAP_PRIVATE | FASTIX_MAP_ANONYMOUS | FASTIX_MAP_FIXED_NOREPLACE,
                           &got);
        if (err != 0) {
            return err == -FASTIX_EEXIST ? -FASTIX_ENOEXEC : err; /* segments share a page */
        }
        err = vms_write(vs, seg.vaddr, (const uint8_t *)img->data + seg.offset, seg.filesz);
        if (err != 0) {
            return err;
        }
        if (end > *brk) {
            *brk = end;
        }
    }
    return *brk != 0 ? 0 : -FASTIX_ENOEXEC;
}

static size_t str_size(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n + 1;
}

static size_t count(const char *const *v)
{
    size_t n = 0;
    while (v != NULL && v[n] != NULL) {
        n++;
    }
    return n;
}

/* Buffered writer of 64-bit words into process memory. */
struct wbuf {
    struct vmspace *vs;
    uint64_t addr;
    uint64_t words[WBUF_WORDS];
    unsigned n;
    int err;
};

static void wbuf_flush(struct wbuf *w)
{
    if (w->err == 0 && w->n > 0) {
        w->err = vms_write(w->vs, w->addr, w->words, w->n * sizeof(uint64_t));
        w->addr += w->n * sizeof(uint64_t);
    }
    w->n = 0;
}

static void wbuf_put(struct wbuf *w, uint64_t word)
{
    w->words[w->n++] = word;
    if (w->n == WBUF_WORDS) {
        wbuf_flush(w);
    }
}

/* Writes the pointers to `strs` (laid out back to back from *cursor) followed by NULL. */
static void put_string_ptrs(struct wbuf *w, const char *const *strs, uint64_t *cursor)
{
    for (size_t i = 0; strs != NULL && strs[i] != NULL; i++) {
        wbuf_put(w, *cursor);
        *cursor += str_size(strs[i]);
    }
    wbuf_put(w, 0);
}

static uint64_t splitmix(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/*
 * Stack layout, from STACK_TOP down: argv strings then envp strings, 16 random
 * bytes, then (16-byte aligned) argc, argv[], NULL, envp[], NULL, auxv pairs.
 */
static int build_stack(struct vmspace *vs, const struct load_args *a, const struct elf_image *img,
                       uint64_t *sp)
{
    size_t argc = count(a->argv), envc = count(a->envp);
    uint64_t strings = 0;
    for (size_t i = 0; i < argc; i++) {
        strings += str_size(a->argv[i]);
    }
    for (size_t i = 0; i < envc; i++) {
        strings += str_size(a->envp[i]);
    }
    if (strings + (argc + envc + 2) * sizeof(uint64_t) > FASTIX_ARG_MAX) {
        return -FASTIX_E2BIG;
    }
    uint64_t str_base = FASTIX_STACK_TOP - strings, at = str_base;
    for (size_t i = 0; i < argc + envc; i++) {
        const char *s = i < argc ? a->argv[i] : a->envp[i - argc];
        int err = vms_write(vs, at, s, str_size(s));
        if (err != 0) {
            return err;
        }
        at += str_size(s);
    }
    uint64_t seed = a->random_seed, random[2] = { splitmix(&seed), splitmix(&seed) };
    uint64_t random_addr = (str_base - sizeof(random)) & ~(uint64_t)15;
    int err = vms_write(vs, random_addr, random, sizeof(random));
    if (err != 0) {
        return err;
    }

    uint64_t words = 1 + (argc + 1) + (envc + 1) + 2 * NUM_AUXV;
    struct wbuf w = { .vs = vs, .addr = (random_addr - words * sizeof(uint64_t)) & ~(uint64_t)15 };
    *sp = w.addr;
    uint64_t cursor = str_base;
    wbuf_put(&w, argc);
    put_string_ptrs(&w, a->argv, &cursor);
    put_string_ptrs(&w, a->envp, &cursor);
    const uint64_t auxv[NUM_AUXV][2] = {
        { FASTIX_AT_PHDR, elf_phdr_addr(img) },  { FASTIX_AT_PHENT, ELF64_PHDR_SIZE },
        { FASTIX_AT_PHNUM, img->phnum },         { FASTIX_AT_PAGESZ, FASTIX_PAGE_SIZE },
        { FASTIX_AT_BASE, 0 },                   { FASTIX_AT_FLAGS, 0 },
        { FASTIX_AT_ENTRY, img->entry },         { FASTIX_AT_UID, a->uid },
        { FASTIX_AT_EUID, a->euid },             { FASTIX_AT_GID, a->gid },
        { FASTIX_AT_EGID, a->egid },             { FASTIX_AT_SECURE, 0 },
        { FASTIX_AT_CLKTCK, FASTIX_CLK_TCK },    { FASTIX_AT_RANDOM, random_addr },
        { FASTIX_AT_EXECFN, argc > 0 ? str_base : 0 },
        { FASTIX_AT_SYSINFO_EHDR, a->vdso_ehdr },
        { FASTIX_AT_NULL, 0 },
    };
    for (unsigned i = 0; i < NUM_AUXV; i++) {
        wbuf_put(&w, auxv[i][0]);
        wbuf_put(&w, auxv[i][1]);
    }
    wbuf_flush(&w);
    return w.err;
}

int loader_load(struct vmspace *vs, const struct load_args *a, uint64_t *entry, uint64_t *sp)
{
    struct elf_image img;
    uint64_t brk;

    int err = elf_open(&img, a->elf, a->elf_size, ARCH_ELF_MACHINE);
    if (err != 0) {
        return err;
    }
    if (img.type == ELF_ET_EXEC) {
        err = elf_set_load(&img, 0, FASTIX_EXEC_BASE, FASTIX_EXEC_END);
    } else {
        err = elf_set_load(&img, FASTIX_PIE_BASE, FASTIX_PIE_BASE, FASTIX_MMAP_BASE);
    }
    if (err != 0 || (err = load_segments(vs, &img, &brk)) != 0) {
        return err;
    }
    vs->brk_start = brk;
    vs->brk_end = brk;
    uint64_t stack;
    err = vms_mmap(vs, FASTIX_STACK_TOP - FASTIX_DEFAULT_STACK_SIZE, FASTIX_DEFAULT_STACK_SIZE,
                   FASTIX_PROT_READ | FASTIX_PROT_WRITE,
                   FASTIX_MAP_PRIVATE | FASTIX_MAP_ANONYMOUS | FASTIX_MAP_FIXED_NOREPLACE, &stack);
    if (err != 0) {
        return err;
    }
    *entry = img.entry;
    return build_stack(vs, a, &img, sp);
}

static bool is_ws(uint8_t c)
{
    return c == ' ' || c == '\t';
}

int loader_parse_shebang(const uint8_t *data, size_t size, char *interp, size_t interp_cap,
                         char *arg, size_t arg_cap)
{
    if (size < 2 || data[0] != '#' || data[1] != '!') {
        return 0;
    }
    size_t line_end = 2;
    while (line_end < size && data[line_end] != '\n' && data[line_end] != '\0') {
        line_end++;
    }
    while (line_end > 2 && (is_ws(data[line_end - 1]) || data[line_end - 1] == '\r')) {
        line_end--;
    }
    size_t i = 2;
    while (i < line_end && is_ws(data[i])) {
        i++;
    }
    size_t interp_start = i;
    while (i < line_end && !is_ws(data[i])) {
        i++;
    }
    size_t interp_len = i - interp_start;
    if (interp_len == 0 || interp_len + 1 > interp_cap) {
        return -FASTIX_ENOEXEC;
    }
    for (size_t k = 0; k < interp_len; k++) {
        interp[k] = (char)data[interp_start + k];
    }
    interp[interp_len] = '\0';

    while (i < line_end && is_ws(data[i])) {
        i++;
    }
    size_t arg_len = line_end - i;
    if (arg_len + 1 > arg_cap) {
        return -FASTIX_ENOEXEC;
    }
    for (size_t k = 0; k < arg_len; k++) {
        arg[k] = (char)data[i + k];
    }
    arg[arg_len] = '\0';
    return 1;
}
