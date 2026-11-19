/*
 * libdriver/dtb.h - bounds-checked Flattened Device Tree (FDT v17) parser.
 * Pure freestanding C17 (no seL4 dependency); tested on host against QEMU's DTB.
 */
#ifndef LIBDRIVER_DTB_H
#define LIBDRIVER_DTB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libos/compiler.h>

/* GIC interrupt types in a 3-cell ARM GIC interrupts property. */
#define DTB_GIC_SPI 0U /* shared peripheral interrupt: hardware IRQ = num + 32 */
#define DTB_GIC_PPI 1U /* private peripheral interrupt: hardware IRQ = num + 16 */

struct dtb_view {
    const uint8_t *base;
    uint32_t totalsize;
    uint32_t off_struct;
    uint32_t size_struct;
    uint32_t off_strings;
    uint32_t size_strings;
};

struct dtb_node {
    size_t offset;              /* offset of FDT_BEGIN_NODE inside struct block */
    size_t props_off;           /* offset of first property/child inside struct block */
    size_t next_off;            /* pass as start_off to continue searching after this node */
    const char *name;           /* NUL-terminated node name inside DTB */
    uint32_t parent_addr_cells; /* parent's #address-cells (1 or 2) */
    uint32_t parent_size_cells; /* parent's #size-cells (0, 1, or 2) */
};

struct dtb_irq {
    uint32_t irq;   /* GIC interrupt ID (SPI + 32 or PPI + 16) */
    uint32_t type;  /* DTB_GIC_SPI or DTB_GIC_PPI */
    uint32_t flags; /* raw GIC trigger flags from DTB */
    bool edge;      /* true if edge-triggered, false if level-triggered */
};

/* Validates an FDT blob in [dtb, dtb + len) and fills *out.
 * Returns 0 or -FASTIX_EINVAL. */
LIBOS_MUST_CHECK int dtb_validate(const void *dtb, size_t len, struct dtb_view *out);

/* Finds the first node at struct-block offset >= start_off whose "compatible"
 * string list contains compat. Pass start_off = 0 for the first call and
 * node.next_off on subsequent calls to iterate all matching nodes.
 * Returns 0, -FASTIX_ENOENT (no more matches), or -FASTIX_EINVAL (corrupt DTB). */
LIBOS_MUST_CHECK int dtb_find_compat(const struct dtb_view *v, const char *compat,
                                     size_t start_off, struct dtb_node *out);

/* Looks up property name on node n (not its children).
 * On success writes *data and *len and returns 0.
 * Returns -FASTIX_ENOENT if absent, or -FASTIX_EINVAL if the node is corrupt. */
LIBOS_MUST_CHECK int dtb_node_prop(const struct dtb_view *v, const struct dtb_node *n,
                                   const char *name, const void **data, uint32_t *len);

/* Reads a big-endian 32-bit integer property from node n. */
LIBOS_MUST_CHECK int dtb_node_u32(const struct dtb_view *v, const struct dtb_node *n,
                                  const char *name, uint32_t *out);

/* Reads entry index from node n's "reg" property using its parent's cell counts.
 * Returns 0, -FASTIX_ENOENT if index is out of range, or -FASTIX_EINVAL. */
LIBOS_MUST_CHECK int dtb_node_reg(const struct dtb_view *v, const struct dtb_node *n,
                                  size_t index, uint64_t *base, uint64_t *size);

/* Reads entry index from node n's 3-cell ARM GIC "interrupts" property.
 * Returns 0, -FASTIX_ENOENT if index is out of range, or -FASTIX_EINVAL. */
LIBOS_MUST_CHECK int dtb_node_irq(const struct dtb_view *v, const struct dtb_node *n,
                                  size_t index, struct dtb_irq *out);

#endif /* LIBDRIVER_DTB_H */
