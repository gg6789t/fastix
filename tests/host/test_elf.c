/*
 * test_elf.c - tests for libos/elf: known-bad headers are rejected, and
 * randomly corrupted images never read out of bounds (ASan) and never yield
 * a segment that breaks the documented bounds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fastix/errno.h>
#include <libos/elf.h>

#include "rng.h"

#define MACHINE   183            /* EM_AARCH64 */
#define LO        0x400000ULL
#define HI        0x40000000ULL
#define IMG_SIZE  512
#define SEEDS     200
#define MUTATIONS 2000

static uint8_t good[IMG_SIZE];
static int failures;

#define EXPECT(cond, what) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s (line %d)\n", what, __LINE__); \
            failures++; \
        } \
    } while (0)

static void put16(uint8_t *p, unsigned off, uint16_t v) { memcpy(p + off, &v, 2); }
static void put32(uint8_t *p, unsigned off, uint32_t v) { memcpy(p + off, &v, 4); }
static void put64(uint8_t *p, unsigned off, uint64_t v) { memcpy(p + off, &v, 8); }

/* ELF header + 2 program headers (one R-X PT_LOAD, one PT_NOTE) + 64 bytes of "code". */
static void build_good(void)
{
    memset(good, 0, sizeof(good));
    memcpy(good, "\x7f" "ELF\x02\x01\x01", 7);
    put16(good, 16, 2);          /* ET_EXEC */
    put16(good, 18, MACHINE);
    put32(good, 20, 1);
    put64(good, 24, LO + 0x100); /* entry */
    put64(good, 32, 64);         /* phoff */
    put16(good, 54, 56);         /* phentsize */
    put16(good, 56, 2);          /* phnum */
    /* phdr 0: PT_LOAD R-X, file [0x100, 0x140) -> mem LO+0x100, 0x1000 bytes */
    put32(good, 64, 1);
    put32(good, 68, ELF_PF_R | ELF_PF_X);
    put64(good, 72, 0x100);
    put64(good, 80, LO + 0x100);
    put64(good, 96, 0x40);
    put64(good, 104, 0x1000);
    /* phdr 1: PT_NOTE (not loaded) */
    put32(good, 120, 4);
}

static int open_copy(uint8_t *img, size_t size, struct elf_image *out)
{
    return elf_open(out, img, size, MACHINE, LO, HI);
}

static void test_good_image(void)
{
    struct elf_image img;
    struct elf_segment seg;

    EXPECT(open_copy(good, sizeof(good), &img) == 0, "good image opens");
    EXPECT(img.entry == LO + 0x100, "entry read");
    EXPECT(elf_segment(&img, 0, &seg) == 1, "PT_LOAD is loadable");
    EXPECT(seg.vaddr == LO + 0x100 && seg.filesz == 0x40 && seg.memsz == 0x1000, "segment fields");
    EXPECT(elf_segment(&img, 1, &seg) == 0, "PT_NOTE is skipped");
    EXPECT(elf_segment(&img, 2, &seg) == -FASTIX_ENOEXEC, "index past phnum rejected");
}

/* Applies one header change and expects elf_open (or segment 0) to fail. */
static void expect_rejected(const char *what, unsigned off, uint64_t value, unsigned width,
                            int check_segment)
{
    uint8_t img[IMG_SIZE];
    struct elf_image e;
    struct elf_segment seg;

    memcpy(img, good, sizeof(img));
    memcpy(img + off, &value, width);
    int r = open_copy(img, sizeof(img), &e);
    if (check_segment) {
        EXPECT(r == 0, what);
        r = (r == 0) ? elf_segment(&e, 0, &seg) : r;
    }
    EXPECT(r == -FASTIX_ENOEXEC, what);
}

static void test_bad_images(void)
{
    struct elf_image e;

    EXPECT(open_copy(good, 63, &e) == -FASTIX_ENOEXEC, "truncated header rejected");
    EXPECT(open_copy(good, 64 + 56, &e) == -FASTIX_ENOEXEC, "truncated phdr table rejected");
    expect_rejected("bad magic", 1, 'X', 1, 0);
    expect_rejected("32-bit class", 4, 1, 1, 0);
    expect_rejected("big endian", 5, 2, 1, 0);
    expect_rejected("ET_DYN", 16, 3, 2, 0);
    expect_rejected("wrong machine", 18, 62, 2, 0);
    expect_rejected("wrong phentsize", 54, 32, 2, 0);
    expect_rejected("zero phnum", 56, 0, 2, 0);
    expect_rejected("huge phnum", 56, 0xFFFF, 2, 0);
    expect_rejected("phoff past end", 32, UINT64_MAX - 8, 8, 0);
    expect_rejected("entry below range", 24, LO - 1, 8, 0);
    expect_rejected("entry above range", 24, HI, 8, 0);
    expect_rejected("filesz > memsz", 96, 0x2000, 8, 1);
    expect_rejected("offset past end", 72, IMG_SIZE + 1, 8, 1);
    expect_rejected("offset+filesz overflow", 72, UINT64_MAX - 0x10, 8, 1);
    expect_rejected("vaddr below range", 80, LO - 0x1000, 8, 1);
    expect_rejected("memsz past range end", 104, UINT64_MAX - 0x100, 8, 1);
    expect_rejected("W+X segment", 68, ELF_PF_R | ELF_PF_W | ELF_PF_X, 4, 1);
}

/* Random byte corruption: must never crash, and accepted segments must obey the bounds. */
static void test_fuzz(void)
{
    uint8_t img[IMG_SIZE];

    for (uint64_t seed = 1; seed <= SEEDS; seed++) {
        uint64_t rng = seed;
        for (int m = 0; m < MUTATIONS; m++) {
            memcpy(img, good, sizeof(img));
            int flips = (int)rng_range(&rng, 1, 8);
            for (int f = 0; f < flips; f++) {
                img[rng_range(&rng, 0, 64 + 2 * 56 - 1)] = (uint8_t)rng_next(&rng);
            }
            size_t size = (size_t)rng_range(&rng, 0, IMG_SIZE);
            struct elf_image e;
            if (open_copy(img, size, &e) != 0) {
                continue;
            }
            for (unsigned i = 0; i < e.phnum; i++) {
                struct elf_segment seg;
                if (elf_segment(&e, i, &seg) != 1) {
                    continue;
                }
                EXPECT(seg.offset + seg.filesz <= size && seg.filesz <= seg.memsz, "file bounds");
                EXPECT(seg.vaddr >= LO && seg.vaddr + seg.memsz <= HI, "address bounds");
                EXPECT(!((seg.flags & ELF_PF_W) && (seg.flags & ELF_PF_X)), "W^X");
            }
        }
    }
}

int main(void)
{
    build_good();
    test_good_image();
    test_bad_images();
    test_fuzz();
    if (failures != 0) {
        fprintf(stderr, "test_elf: %d failures\n", failures);
        return 1;
    }
    printf("test_elf: good/bad cases passed, %d seeds x %d random corruptions passed\n", SEEDS,
           MUTATIONS);
    return 0;
}
