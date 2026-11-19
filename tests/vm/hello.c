/*
 * hello.c - checks what the loader hands a new program: argc/argv/envp,
 * the auxiliary vector, and working .data/.bss. Built twice: as a
 * fixed-address executable and (with HELLO_PIE) as a static PIE.
 * Exits 42 if everything is right, otherwise the number of the first failed check.
 * The PIE build first applies its own relative relocations, the way musl's
 * static-PIE startup does, using only what the loader passed (AT_PHDR).
 */
#include <stdint.h>

#include <fastix/abi.h>
#include <fastix/vspace.h>
#include <fastix_sys/sys.h>

#ifdef HELLO_PIE
#define EXPECT_NAME "hello_pie"
#else
#define EXPECT_NAME "hello_static"
#endif

void _start(void);
_Noreturn void prog_main(uint64_t *sp);

static int data_value = 5; /* lives in .data */
static int bss_value;      /* lives in .bss */

static int str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint64_t aux_get(const uint64_t *aux, uint64_t key)
{
    for (; aux[0] != FASTIX_AT_NULL; aux += 2) {
        if (aux[0] == key) {
            return aux[1];
        }
    }
    return 0;
}

#define PT_LOAD            1
#define PT_DYNAMIC         2
#define DT_NULL            0
#define DT_RELA            7
#define DT_RELASZ          8
#define R_AARCH64_RELATIVE 1027

/* Skips argc, argv and envp to reach the auxiliary vector. */
static const uint64_t *find_auxv(uint64_t *sp)
{
    const uint64_t *p = sp + 1 + sp[0] + 1; /* past argc, argv[], NULL */
    while (*p != 0) {
        p++;
    }
    return p + 1;
}

#ifdef HELLO_PIE
/* Defined by the linker; hidden so it is reached PC-relative, before any relocation. */
extern const uint64_t _DYNAMIC[] __attribute__((visibility("hidden")));

/* Applies R_AARCH64_RELATIVE relocations. Touches no global data until done. */
static void self_relocate(uint64_t *sp)
{
    const uint64_t *aux = find_auxv(sp);
    uint64_t phdr = aux_get(aux, FASTIX_AT_PHDR), phnum = aux_get(aux, FASTIX_AT_PHNUM);
    uint64_t base = 0;
    for (uint64_t i = 0; i < phnum; i++) {
        const uint64_t *ph = (const uint64_t *)(uintptr_t)(phdr + i * 56);
        if ((uint32_t)ph[0] == PT_DYNAMIC) {
            base = (uint64_t)(uintptr_t)_DYNAMIC - ph[2]; /* runtime minus link-time address */
        }
    }
    uint64_t rela = 0, relasz = 0;
    for (const uint64_t *d = _DYNAMIC; d[0] != DT_NULL; d += 2) {
        rela = d[0] == DT_RELA ? d[1] : rela;
        relasz = d[0] == DT_RELASZ ? d[1] : relasz;
    }
    for (uint64_t off = 0; off < relasz; off += 24) {
        const uint64_t *r = (const uint64_t *)(uintptr_t)(base + rela + off);
        if ((uint32_t)r[1] == R_AARCH64_RELATIVE) {
            *(uint64_t *)(uintptr_t)(base + r[0]) = base + r[2];
        }
    }
}
#endif

static int has_load_segment(uint64_t phdr, uint64_t phnum)
{
    for (uint64_t i = 0; i < phnum; i++) {
        if (*(const uint32_t *)(uintptr_t)(phdr + i * 56) == PT_LOAD) {
            return 1;
        }
    }
    return 0;
}

static int check(uint64_t *sp)
{
    uint64_t argc = sp[0];
    const char **argv = (const char **)(sp + 1);
    const char **envp = argv + argc + 1;
    const uint64_t *aux = find_auxv(sp);
    uint64_t entry = (uint64_t)(uintptr_t)&_start;

    if (argc != 1 || argv[1] != 0 || !str_eq(argv[0], EXPECT_NAME)) return 1;
    if (envp[0] == 0 || !str_eq(envp[0], "FASTIX=1") || envp[1] != 0) return 2;
    if (aux_get(aux, FASTIX_AT_PAGESZ) != 4096 || aux_get(aux, FASTIX_AT_PHENT) != 56) return 3;
    if (aux_get(aux, FASTIX_AT_ENTRY) != entry) return 4;
    uint64_t phdr = aux_get(aux, FASTIX_AT_PHDR), phnum = aux_get(aux, FASTIX_AT_PHNUM);
    if (phdr == 0 || phnum == 0 || !has_load_segment(phdr, phnum)) return 5;
    if (aux_get(aux, FASTIX_AT_RANDOM) == 0) return 6;
    if (aux_get(aux, FASTIX_AT_EXECFN) != (uint64_t)(uintptr_t)argv[0]) return 7;
    if (((uint64_t)(uintptr_t)sp & 15) != 0) return 8;
#ifdef HELLO_PIE
    if (entry < FASTIX_PIE_BASE || entry >= FASTIX_MMAP_BASE) return 9;
#else
    if (entry < FASTIX_EXEC_BASE || entry >= FASTIX_EXEC_END) return 9;
#endif
    if (data_value != 5 || bss_value != 0) return 10;
    data_value = 6;
    bss_value = 1;
    if (data_value != 6 || bss_value != 1) return 11;
    return 42;
}

_Noreturn void prog_main(uint64_t *sp)
{
#ifdef HELLO_PIE
    self_relocate(sp);
#endif
    fastix_sys_init();
    (void)fastix_sys1(FASTIX_SYS_exit_group, (uint64_t)check(sp));
    for (;;) {
    }
}
