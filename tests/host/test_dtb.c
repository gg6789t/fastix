/*
 * test_dtb.c - host tests for libdriver/dtb on QEMU's real dumped DTB plus
 * 200 seeds of fuzz/corruption inputs (ASan + UBSan).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fastix/errno.h>
#include <libdriver/dtb.h>

#include "rng.h"

#define SEEDS 200

static uint64_t seed;
static int step;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL seed %llu step %d: %s (line %d)\n", \
                    (unsigned long long)seed, step, #cond, __LINE__); \
            exit(1); \
        } \
    } while (0)

static uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "FAIL: cannot open DTB file: %s (run tools/build.sh first)\n", path);
        exit(1);
    }
    CHECK(fseek(f, 0, SEEK_END) == 0);
    long sz = ftell(f);
    CHECK(sz > 0);
    CHECK(fseek(f, 0, SEEK_SET) == 0);
    uint8_t *buf = malloc((size_t)sz);
    CHECK(buf != NULL);
    CHECK(fread(buf, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);
    *len_out = (size_t)sz;
    return buf;
}

static void check_qemu_dtb(const uint8_t *dtb, size_t len)
{
    struct dtb_view v;
    CHECK(dtb_validate(dtb, len, &v) == 0);
    CHECK(v.totalsize <= len);

    /* PL011 UART */
    struct dtb_node n;
    uint64_t base = 0, size = 0;
    struct dtb_irq irq;
    CHECK(dtb_find_compat(&v, "arm,pl011", 0, &n) == 0);
    CHECK(dtb_node_reg(&v, &n, 0, &base, &size) == 0);
    CHECK(base == 0x09000000ULL && size == 0x1000ULL);
    CHECK(dtb_node_reg(&v, &n, 1, &base, &size) == -FASTIX_ENOENT);
    CHECK(dtb_node_irq(&v, &n, 0, &irq) == 0);
    CHECK(irq.type == DTB_GIC_SPI && irq.irq == 33U && !irq.edge);
    CHECK(dtb_node_irq(&v, &n, 1, &irq) == -FASTIX_ENOENT);
    CHECK(dtb_find_compat(&v, "arm,pl011", n.next_off, &n) == -FASTIX_ENOENT);

    /* PL031 RTC */
    CHECK(dtb_find_compat(&v, "arm,pl031", 0, &n) == 0);
    CHECK(dtb_node_reg(&v, &n, 0, &base, &size) == 0);
    CHECK(base == 0x09010000ULL && size == 0x1000ULL);
    CHECK(dtb_node_irq(&v, &n, 0, &irq) == 0);
    CHECK(irq.type == DTB_GIC_SPI && irq.irq == 34U && !irq.edge);

    /* VirtIO MMIO: QEMU virt exposes 32 slots at 0x0a000000..0x0a003e00, IRQs 48..79 */
    bool seen_slot[32] = { false };
    size_t cursor = 0;
    int virtio_count = 0;
    while (dtb_find_compat(&v, "virtio,mmio", cursor, &n) == 0) {
        CHECK(dtb_node_reg(&v, &n, 0, &base, &size) == 0);
        CHECK(size == 0x200ULL);
        CHECK(base >= 0x0a000000ULL && base < 0x0a004000ULL && ((base - 0x0a000000ULL) % 0x200ULL) == 0);
        size_t idx = (size_t)((base - 0x0a000000ULL) / 0x200ULL);
        CHECK(idx < 32 && !seen_slot[idx]);
        seen_slot[idx] = true;
        CHECK(dtb_node_irq(&v, &n, 0, &irq) == 0);
        CHECK(irq.type == DTB_GIC_SPI && irq.irq == 48U + (uint32_t)idx && irq.edge);
        virtio_count++;
        cursor = n.next_off;
    }
    CHECK(virtio_count == 32);

    /* ARMv8 generic timer: 4 PPIs (29, 30, 27, 26) */
    CHECK(dtb_find_compat(&v, "arm,armv8-timer", 0, &n) == 0);
    static const uint32_t expected_timer_irqs[4] = { 29U, 30U, 27U, 26U };
    for (size_t i = 0; i < 4; i++) {
        CHECK(dtb_node_irq(&v, &n, i, &irq) == 0);
        CHECK(irq.type == DTB_GIC_PPI && irq.irq == expected_timer_irqs[i] && !irq.edge);
    }
    CHECK(dtb_node_irq(&v, &n, 4, &irq) == -FASTIX_ENOENT);

    /* GICv2 interrupt controller: 2 reg entries + #interrupt-cells == 3 */
    CHECK(dtb_find_compat(&v, "arm,cortex-a15-gic", 0, &n) == 0);
    CHECK(dtb_node_reg(&v, &n, 0, &base, &size) == 0);
    CHECK(base == 0x08000000ULL && size == 0x10000ULL);
    CHECK(dtb_node_reg(&v, &n, 1, &base, &size) == 0);
    CHECK(base == 0x08010000ULL && size == 0x10000ULL);
    uint32_t icells = 0;
    CHECK(dtb_node_u32(&v, &n, "#interrupt-cells", &icells) == 0 && icells == 3U);

    /* Fixed APB clock: 24 MHz */
    CHECK(dtb_find_compat(&v, "fixed-clock", 0, &n) == 0);
    uint32_t freq = 0;
    CHECK(dtb_node_u32(&v, &n, "clock-frequency", &freq) == 0 && freq == 24000000U);

    /* Non-existent device */
    CHECK(dtb_find_compat(&v, "fastix,nonexistent-device", 0, &n) == -FASTIX_ENOENT);
}

static void exercise_untrusted(const uint8_t *buf, size_t len)
{
    struct dtb_view v;
    if (dtb_validate(buf, len, &v) != 0) {
        return;
    }
    struct dtb_node n;
    size_t cursor = 0;
    for (int iters = 0; iters < 64; iters++) {
        int err = dtb_find_compat(&v, "virtio,mmio", cursor, &n);
        if (err != 0) {
            break;
        }
        uint64_t base = 0, size = 0;
        struct dtb_irq irq;
        uint32_t u = 0;
        (void)dtb_node_reg(&v, &n, 0, &base, &size);
        (void)dtb_node_irq(&v, &n, 0, &irq);
        (void)dtb_node_u32(&v, &n, "#address-cells", &u);
        if (n.next_off <= cursor) {
            break;
        }
        cursor = n.next_off;
    }
}

static void check_corrupt_and_fuzz(const uint8_t *orig, size_t len)
{
    struct dtb_view v;
    CHECK(dtb_validate(NULL, len, &v) == -FASTIX_EINVAL);
    CHECK(dtb_validate(orig, 0, &v) == -FASTIX_EINVAL);
    CHECK(dtb_validate(orig, 39, &v) == -FASTIX_EINVAL);
    CHECK(dtb_validate(orig, len - 1, &v) == -FASTIX_EINVAL);

    uint8_t *mut = malloc(len);
    CHECK(mut != NULL);

    /* Bad magic must fail validation. */
    memcpy(mut, orig, len);
    mut[0] ^= 0xff;
    CHECK(dtb_validate(mut, len, &v) == -FASTIX_EINVAL);

    /* 200 seeds of random truncations, header corruptions, and body mutations. */
    for (seed = 1; seed <= SEEDS; seed++) {
        uint64_t rng = seed;
        for (step = 0; step < 25; step++) {
            memcpy(mut, orig, len);
            int mode = (int)rng_range(&rng, 0, 3);
            if (mode == 0) {
                /* Truncate buffer to a random length. */
                size_t cut = (size_t)rng_range(&rng, 0, len - 1);
                exercise_untrusted(mut, cut);
            } else if (mode == 1) {
                /* Corrupt random bytes in the header (offsets 4..39). */
                size_t off = (size_t)rng_range(&rng, 4, 39);
                mut[off] = (uint8_t)rng_next(&rng);
                exercise_untrusted(mut, len);
            } else if (mode == 2) {
                /* Corrupt 1..8 random bytes anywhere in the DTB body. */
                int nflips = (int)rng_range(&rng, 1, 8);
                for (int k = 0; k < nflips; k++) {
                    size_t off = (size_t)rng_range(&rng, 40, len - 1);
                    mut[off] ^= (uint8_t)(1U << rng_range(&rng, 0, 7));
                }
                exercise_untrusted(mut, len);
            } else {
                /* Overwrite a 4-byte word in the struct block with a hostile value. */
                size_t off = 40 + ((size_t)rng_range(&rng, 0, (len - 44) / 4) * 4);
                uint32_t bad = (uint32_t)rng_next(&rng);
                memcpy(mut + off, &bad, 4);
                exercise_untrusted(mut, len);
            }
        }
    }
    free(mut);
}

int main(void)
{
    size_t len = 0;
    uint8_t *dtb = read_file(FASTIX_QEMU_DTB_PATH, &len);
    check_qemu_dtb(dtb, len);
    check_corrupt_and_fuzz(dtb, len);
    free(dtb);
    printf("test_dtb: QEMU DTB verified + %d fuzz seeds passed\n", SEEDS);
    return 0;
}
