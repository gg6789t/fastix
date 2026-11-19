/*
 * libs/libdriver/dtb.c - bounds-checked Flattened Device Tree (FDT v17) parser.
 */
#include <fastix/errno.h>
#include <libdriver/dtb.h>

/* FDT specification constants (Devicetree Specification v0.4, Section 5). */
#define FDT_MAGIC        0xd00dfeedU
#define FDT_HEADER_SIZE  40U
#define FDT_MIN_VERSION  17U
#define FDT_BEGIN_NODE   1U
#define FDT_END_NODE     2U
#define FDT_PROP         3U
#define FDT_NOP          4U
#define FDT_END          9U

#define DTB_MAX_DEPTH    16U
#define GIC_SPI_BASE     32U
#define GIC_PPI_BASE     16U
#define GIC_EDGE_MASK    0x3U

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static bool align4_add(size_t pos, size_t add, size_t limit, size_t *out)
{
    if (add > limit - pos) {
        return false;
    }
    size_t end = pos + add;
    size_t pad = (4U - (end & 3U)) & 3U;
    if (pad > limit - end) {
        return false;
    }
    *out = end + pad;
    return true;
}

/* Looks up a NUL-terminated string in the FDT strings block with strict bounds. */
static const char *dtb_string(const struct dtb_view *v, uint32_t nameoff)
{
    if (nameoff >= v->size_strings) {
        return NULL;
    }
    const uint8_t *s = v->base + v->off_strings + nameoff;
    uint32_t max_len = v->size_strings - nameoff;

    for (uint32_t i = 0; i < max_len; i++) {
        if (s[i] == '\0') {
            return (const char *)s;
        }
    }
    return NULL;
}

/* True if a NUL-separated compatible list [data, data + len) contains target. */
static bool compat_list_contains(const uint8_t *data, uint32_t len, const char *target)
{
    if (len == 0 || data[len - 1] != '\0') {
        return false;
    }
    uint32_t i = 0;
    while (i < len) {
        const char *entry = (const char *)(data + i);
        if (str_eq(entry, target)) {
            return true;
        }
        while (i < len && data[i] != '\0') {
            i++;
        }
        i++;
    }
    return false;
}

/* Advances past FDT_BEGIN_NODE name; writes NUL-terminated name and next offset. */
static int parse_node_name(const uint8_t *sb, size_t slen, size_t pos,
                           const char **name_out, size_t *next_out)
{
    size_t i = pos;
    while (i < slen && sb[i] != '\0') {
        i++;
    }
    if (i >= slen) {
        return -FASTIX_EINVAL;
    }
    size_t name_bytes = (i - pos) + 1U;
    if (!align4_add(pos, name_bytes, slen, next_out)) {
        return -FASTIX_EINVAL;
    }
    *name_out = (const char *)(sb + pos);
    return 0;
}

/* Parses an FDT_PROP payload starting right after the FDT_PROP token. */
static int parse_prop(const struct dtb_view *v, const uint8_t *sb, size_t slen, size_t pos,
                      const char **name_out, const uint8_t **data_out,
                      uint32_t *len_out, size_t *next_out)
{
    if (pos > slen || slen - pos < 8U) {
        return -FASTIX_EINVAL;
    }
    uint32_t plen = read_be32(sb + pos);
    uint32_t nameoff = read_be32(sb + pos + 4U);
    pos += 8U;

    const char *name = dtb_string(v, nameoff);
    if (name == NULL) {
        return -FASTIX_EINVAL;
    }
    if (!align4_add(pos, plen, slen, next_out)) {
        return -FASTIX_EINVAL;
    }
    *name_out = name;
    *data_out = sb + pos;
    *len_out = plen;
    return 0;
}

int dtb_validate(const void *dtb, size_t len, struct dtb_view *out)
{
    if (dtb == NULL || out == NULL || len < FDT_HEADER_SIZE) {
        return -FASTIX_EINVAL;
    }
    const uint8_t *b = dtb;
    uint32_t magic = read_be32(b + 0U);
    uint32_t totalsize = read_be32(b + 4U);
    uint32_t off_struct = read_be32(b + 8U);
    uint32_t off_strings = read_be32(b + 12U);
    uint32_t version = read_be32(b + 20U);
    uint32_t last_comp = read_be32(b + 24U);
    uint32_t size_strings = read_be32(b + 32U);
    uint32_t size_struct = read_be32(b + 36U);

    if (magic != FDT_MAGIC || totalsize < FDT_HEADER_SIZE || totalsize > len) {
        return -FASTIX_EINVAL;
    }
    if (version < FDT_MIN_VERSION || last_comp > FDT_MIN_VERSION) {
        return -FASTIX_EINVAL;
    }
    if ((off_struct & 3U) != 0 || (size_struct & 3U) != 0 || size_struct < 4U) {
        return -FASTIX_EINVAL;
    }
    if (off_struct > totalsize || size_struct > totalsize - off_struct) {
        return -FASTIX_EINVAL;
    }
    if (off_strings > totalsize || size_strings > totalsize - off_strings) {
        return -FASTIX_EINVAL;
    }

    out->base = b;
    out->totalsize = totalsize;
    out->off_struct = off_struct;
    out->size_struct = size_struct;
    out->off_strings = off_strings;
    out->size_strings = size_strings;
    return 0;
}

int dtb_find_compat(const struct dtb_view *v, const char *compat,
                    size_t start_off, struct dtb_node *out)
{
    if (v == NULL || v->base == NULL || compat == NULL || out == NULL) {
        return -FASTIX_EINVAL;
    }
    const uint8_t *sb = v->base + v->off_struct;
    size_t slen = v->size_struct;
    size_t pos = 0;

    uint32_t addr_cells[DTB_MAX_DEPTH];
    uint32_t size_cells[DTB_MAX_DEPTH];
    struct dtb_node stack[DTB_MAX_DEPTH];
    unsigned depth = 0;

    while (pos <= slen - 4U) {
        size_t tok_off = pos;
        uint32_t tok = read_be32(sb + pos);
        pos += 4U;

        if (tok == FDT_NOP) {
            continue;
        }
        if (tok == FDT_END) {
            return depth == 0 ? -FASTIX_ENOENT : -FASTIX_EINVAL;
        }
        if (tok == FDT_BEGIN_NODE) {
            if (depth >= DTB_MAX_DEPTH) {
                return -FASTIX_EINVAL;
            }
            const char *name = NULL;
            int err = parse_node_name(sb, slen, pos, &name, &pos);
            if (err != 0) {
                return err;
            }
            /* Default cell counts per Devicetree spec: 2 address cells, 1 size cell. */
            addr_cells[depth] = 2U;
            size_cells[depth] = 1U;
            stack[depth].offset = tok_off;
            stack[depth].props_off = pos;
            stack[depth].next_off = pos;
            stack[depth].name = name;
            stack[depth].parent_addr_cells = depth > 0 ? addr_cells[depth - 1] : 2U;
            stack[depth].parent_size_cells = depth > 0 ? size_cells[depth - 1] : 1U;
            depth++;
        } else if (tok == FDT_END_NODE) {
            if (depth == 0) {
                return -FASTIX_EINVAL;
            }
            depth--;
        } else if (tok == FDT_PROP) {
            if (depth == 0) {
                return -FASTIX_EINVAL;
            }
            const char *pname = NULL;
            const uint8_t *pdata = NULL;
            uint32_t plen = 0;
            int err = parse_prop(v, sb, slen, pos, &pname, &pdata, &plen, &pos);
            if (err != 0) {
                return err;
            }
            unsigned cur = depth - 1;
            if (plen == 4U && str_eq(pname, "#address-cells")) {
                addr_cells[cur] = read_be32(pdata);
            } else if (plen == 4U && str_eq(pname, "#size-cells")) {
                size_cells[cur] = read_be32(pdata);
            } else if (stack[cur].offset >= start_off && str_eq(pname, "compatible") &&
                       compat_list_contains(pdata, plen, compat)) {
                *out = stack[cur];
                return 0;
            }
        } else {
            return -FASTIX_EINVAL;
        }
    }
    return -FASTIX_EINVAL;
}

int dtb_node_prop(const struct dtb_view *v, const struct dtb_node *n,
                  const char *name, const void **data, uint32_t *len)
{
    if (v == NULL || v->base == NULL || n == NULL || name == NULL ||
        data == NULL || len == NULL || n->props_off > v->size_struct ||
        (n->props_off & 3U) != 0) {
        return -FASTIX_EINVAL;
    }
    const uint8_t *sb = v->base + v->off_struct;
    size_t slen = v->size_struct;
    size_t pos = n->props_off;

    while (pos <= slen - 4U) {
        uint32_t tok = read_be32(sb + pos);
        pos += 4U;

        if (tok == FDT_NOP) {
            continue;
        }
        if (tok == FDT_BEGIN_NODE || tok == FDT_END_NODE) {
            return -FASTIX_ENOENT;
        }
        if (tok != FDT_PROP) {
            return -FASTIX_EINVAL;
        }
        const char *pname = NULL;
        const uint8_t *pdata = NULL;
        uint32_t plen = 0;
        int err = parse_prop(v, sb, slen, pos, &pname, &pdata, &plen, &pos);
        if (err != 0) {
            return err;
        }
        if (str_eq(pname, name)) {
            *data = pdata;
            *len = plen;
            return 0;
        }
    }
    return -FASTIX_EINVAL;
}

int dtb_node_u32(const struct dtb_view *v, const struct dtb_node *n,
                 const char *name, uint32_t *out)
{
    if (out == NULL) {
        return -FASTIX_EINVAL;
    }
    const void *data = NULL;
    uint32_t len = 0;
    int err = dtb_node_prop(v, n, name, &data, &len);
    if (err != 0) {
        return err;
    }
    if (len != 4U) {
        return -FASTIX_EINVAL;
    }
    *out = read_be32(data);
    return 0;
}

static uint64_t read_cells(const uint8_t *p, uint32_t ncells)
{
    uint64_t val = 0;
    for (uint32_t i = 0; i < ncells; i++) {
        val = (val << 32) | read_be32(p + (size_t)i * 4U);
    }
    return val;
}

int dtb_node_reg(const struct dtb_view *v, const struct dtb_node *n,
                 size_t index, uint64_t *base, uint64_t *size)
{
    if (n == NULL || base == NULL || size == NULL) {
        return -FASTIX_EINVAL;
    }
    if (n->parent_addr_cells < 1U || n->parent_addr_cells > 2U || n->parent_size_cells > 2U) {
        return -FASTIX_EINVAL;
    }
    const void *data = NULL;
    uint32_t len = 0;
    int err = dtb_node_prop(v, n, "reg", &data, &len);
    if (err != 0) {
        return err;
    }
    size_t entry_bytes = (size_t)(n->parent_addr_cells + n->parent_size_cells) * 4U;
    if (entry_bytes == 0 || (len % entry_bytes) != 0) {
        return -FASTIX_EINVAL;
    }
    size_t count = len / entry_bytes;
    if (index >= count) {
        return -FASTIX_ENOENT;
    }
    const uint8_t *p = (const uint8_t *)data + index * entry_bytes;
    *base = read_cells(p, n->parent_addr_cells);
    *size = read_cells(p + (size_t)n->parent_addr_cells * 4U, n->parent_size_cells);
    return 0;
}

int dtb_node_irq(const struct dtb_view *v, const struct dtb_node *n,
                 size_t index, struct dtb_irq *out)
{
    if (n == NULL || out == NULL) {
        return -FASTIX_EINVAL;
    }
    const void *data = NULL;
    uint32_t len = 0;
    int err = dtb_node_prop(v, n, "interrupts", &data, &len);
    if (err != 0) {
        return err;
    }
    size_t entry_bytes = 3U * 4U;
    if ((len % entry_bytes) != 0) {
        return -FASTIX_EINVAL;
    }
    size_t count = len / entry_bytes;
    if (index >= count) {
        return -FASTIX_ENOENT;
    }
    const uint8_t *p = (const uint8_t *)data + index * entry_bytes;
    uint32_t type = read_be32(p);
    uint32_t num = read_be32(p + 4U);
    uint32_t flags = read_be32(p + 8U);

    uint32_t base_id;
    if (type == DTB_GIC_SPI) {
        base_id = GIC_SPI_BASE;
    } else if (type == DTB_GIC_PPI) {
        base_id = GIC_PPI_BASE;
    } else {
        return -FASTIX_EINVAL;
    }
    if (num > UINT32_MAX - base_id) {
        return -FASTIX_EINVAL;
    }
    out->irq = base_id + num;
    out->type = type;
    out->flags = flags;
    out->edge = (flags & GIC_EDGE_MASK) != 0;
    return 0;
}
