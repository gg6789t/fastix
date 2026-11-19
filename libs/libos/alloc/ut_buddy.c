/* ut_buddy.c - buddy allocator over untyped capabilities (see libos/ut_buddy.h). */
#include <libos/assert.h>
#include <libos/ut_buddy.h>

#include <fastix/errno.h>

static uint64_t block_size(const struct ut_block *b)
{
    return 1ULL << b->bits;
}

static struct ut_block *node_get(struct ut_buddy *ub)
{
    struct list_node *n = list_pop(&ub->spare);

    if (n == NULL) {
        return NULL;
    }
    ub->blocks_in_use++;
    return LIST_ENTRY(n, struct ut_block, link);
}

static void node_put(struct ut_buddy *ub, struct ut_block *b)
{
    list_push(&ub->spare, &b->link);
    ub->blocks_in_use--;
}

static void free_list_add(struct ut_buddy *ub, struct ut_block *b)
{
    b->state = UT_FREE;
    list_push(&ub->free_list[b->bits], &b->link);
    ub->free_bytes += block_size(b);
}

static void free_list_del(struct ut_buddy *ub, struct ut_block *b)
{
    list_remove(&b->link);
    ub->free_bytes -= block_size(b);
}

/*
 * `b` is free, off every list, and has no derived caps. Merge it with its buddy
 * for as long as the buddy is free too, then put the result on a free list.
 */
static void coalesce(struct ut_buddy *ub, struct ut_block *b)
{
    while (b->parent != NULL) {
        struct ut_block *p = b->parent;
        struct ut_block *buddy = (p->child[0] == b) ? p->child[1] : p->child[0];

        if (buddy->state != UT_FREE) {
            break;
        }
        free_list_del(ub, buddy);
        if (ub->be.revoke(ub->be.ctx, p->slot) != 0) {
            free_list_add(ub, buddy); /* cannot merge now; both halves stay free */
            break;
        }
        slot_alloc_put(ub->slots, b->slot);
        slot_alloc_put(ub->slots, buddy->slot);
        node_put(ub, b);
        node_put(ub, buddy);
        p->child[0] = NULL;
        p->child[1] = NULL;
        b = p;
    }
    free_list_add(ub, b);
}

/*
 * Splits free block `b` (off every list) into halves. On success `b` becomes
 * UT_SPLIT, the upper half goes on a free list, and the lower half is returned
 * in *lower (off every list). On failure nothing changes and `b` has no children.
 */
static int split(struct ut_buddy *ub, struct ut_block *b, struct ut_block **lower)
{
    struct ut_block *half[2] = { node_get(ub), node_get(ub) };
    uint64_t slot[2];
    int err = -FASTIX_ENOMEM;

    if (half[0] == NULL || half[1] == NULL) {
        goto out_nodes;
    }
    err = slot_alloc_get(ub->slots, &slot[0]);
    if (err != 0) {
        goto out_nodes;
    }
    err = slot_alloc_get(ub->slots, &slot[1]);
    if (err != 0) {
        goto out_slot0;
    }
    for (int i = 0; i < 2; i++) {
        err = ub->be.split(ub->be.ctx, b->slot, slot[i], b->bits - 1u);
        if (err != 0) {
            if (i == 1 && ub->be.revoke(ub->be.ctx, b->slot) != 0) {
                libos_assert(0 && "cannot undo half-finished split");
            }
            goto out_slot1;
        }
    }
    for (int i = 0; i < 2; i++) {
        half[i]->parent = b;
        half[i]->child[0] = NULL;
        half[i]->child[1] = NULL;
        half[i]->slot = slot[i];
        half[i]->bits = (uint8_t)(b->bits - 1u);
        half[i]->paddr = b->paddr + (uint64_t)i * (block_size(b) / 2);
        b->child[i] = half[i];
    }
    b->state = UT_SPLIT;
    free_list_add(ub, half[1]);
    *lower = half[0];
    return 0;

out_slot1:
    slot_alloc_put(ub->slots, slot[1]);
out_slot0:
    slot_alloc_put(ub->slots, slot[0]);
out_nodes:
    for (int i = 0; i < 2; i++) {
        if (half[i] != NULL) {
            node_put(ub, half[i]);
        }
    }
    return err;
}

void ut_buddy_init(struct ut_buddy *ub, struct ut_block *nodes, size_t n,
                   struct slot_alloc *slots, struct ut_backend be)
{
    for (int i = 0; i <= UT_MAX_BITS; i++) {
        list_init(&ub->free_list[i]);
    }
    list_init(&ub->spare);
    for (size_t i = 0; i < n; i++) {
        list_push(&ub->spare, &nodes[i].link);
    }
    ub->slots = slots;
    ub->be = be;
    ub->free_bytes = 0;
    ub->blocks_in_use = 0;
}

int ut_buddy_add(struct ut_buddy *ub, uint64_t slot, uint64_t paddr, unsigned bits)
{
    if (bits < UT_MIN_BITS || bits > UT_MAX_BITS) {
        return -FASTIX_EINVAL;
    }
    struct ut_block *b = node_get(ub);
    if (b == NULL) {
        return -FASTIX_ENOMEM;
    }
    b->parent = NULL;
    b->child[0] = NULL;
    b->child[1] = NULL;
    b->slot = slot;
    b->paddr = paddr;
    b->bits = (uint8_t)bits;
    free_list_add(ub, b);
    return 0;
}

int ut_buddy_alloc(struct ut_buddy *ub, unsigned bits, struct ut_block **out)
{
    if (bits < UT_MIN_BITS || bits > UT_MAX_BITS) {
        return -FASTIX_EINVAL;
    }
    unsigned k = bits;
    while (k <= UT_MAX_BITS && list_empty(&ub->free_list[k])) {
        k++;
    }
    if (k > UT_MAX_BITS) {
        return -FASTIX_ENOMEM;
    }
    struct ut_block *b = LIST_ENTRY(ub->free_list[k].next, struct ut_block, link);
    free_list_del(ub, b);

    while (b->bits > bits) {
        struct ut_block *lower;
        int err = split(ub, b, &lower);
        if (err != 0) {
            coalesce(ub, b); /* put back what we took, merging with any free buddy */
            return err;
        }
        b = lower;
    }
    b->state = UT_USED;
    *out = b;
    return 0;
}

int ut_buddy_free(struct ut_buddy *ub, struct ut_block *b)
{
    libos_assert(b->state == UT_USED);
    int err = ub->be.revoke(ub->be.ctx, b->slot);
    if (err != 0) {
        return err;
    }
    coalesce(ub, b);
    return 0;
}
