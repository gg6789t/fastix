/*
 * libs/libos/ds/radix.c - 16-way sparse radix tree with automatic pruning.
 */
#include <stdbool.h>

#include <fastix/errno.h>
#include <libos/radix.h>

struct radix_node {
    void *slots[RADIX_FANOUT];
    uint16_t count;
};

static uint64_t max_index_for_height(unsigned height)
{
    if (height == 0) {
        return 0;
    }
    if (height >= RADIX_MAX_HEIGHT) {
        return UINT64_MAX;
    }
    return (1ULL << (height * RADIX_BITS)) - 1ULL;
}

static unsigned height_needed(uint64_t index)
{
    unsigned h = 1;
    while (h < RADIX_MAX_HEIGHT && index > max_index_for_height(h)) {
        h++;
    }
    return h;
}

static unsigned slot_at(uint64_t index, unsigned level)
{
    return (unsigned)((index >> (level * RADIX_BITS)) & RADIX_MASK);
}

static void shrink_root(struct radix_tree *t)
{
    while (t->height > 1 && t->root != NULL && t->root->count == 1 &&
           t->root->slots[0] != NULL) {
        struct radix_node *old = t->root;
        t->root = old->slots[0];
        slab_free(&t->node_cache, old);
        t->height--;
    }
    if (t->root != NULL && t->root->count == 0) {
        slab_free(&t->node_cache, t->root);
        t->root = NULL;
        t->height = 0;
    }
}

void radix_init(struct radix_tree *t, struct slab_provider provider)
{
    t->root = NULL;
    t->height = 0;
    t->count = 0;
    slab_init(&t->node_cache, "radix_node", sizeof(struct radix_node), provider);
}

void *radix_get(const struct radix_tree *t, uint64_t index)
{
    if (t == NULL || t->root == NULL || index > max_index_for_height(t->height)) {
        return NULL;
    }
    const struct radix_node *node = t->root;
    for (unsigned lvl = t->height - 1; lvl > 0; lvl--) {
        node = node->slots[slot_at(index, lvl)];
        if (node == NULL) {
            return NULL;
        }
    }
    return node->slots[slot_at(index, 0)];
}

static int grow_height(struct radix_tree *t, unsigned target_h)
{
    if (t->root == NULL) {
        struct radix_node *n = slab_alloc(&t->node_cache);
        if (n == NULL) {
            return -FASTIX_ENOMEM;
        }
        t->root = n;
        t->height = target_h;
        return 0;
    }
    while (t->height < target_h) {
        struct radix_node *parent = slab_alloc(&t->node_cache);
        if (parent == NULL) {
            shrink_root(t);
            return -FASTIX_ENOMEM;
        }
        parent->slots[0] = t->root;
        parent->count = 1;
        t->root = parent;
        t->height++;
    }
    return 0;
}

static void prune_path(struct radix_tree *t, struct radix_node **path, const unsigned *slots,
                       unsigned top_lvl, unsigned bottom_lvl)
{
    for (unsigned lvl = bottom_lvl; lvl < top_lvl; lvl++) {
        if (path[lvl]->count != 0) {
            break;
        }
        slab_free(&t->node_cache, path[lvl]);
        path[lvl + 1]->slots[slots[lvl + 1]] = NULL;
        path[lvl + 1]->count--;
    }
    shrink_root(t);
}

int radix_set(struct radix_tree *t, uint64_t index, void *val, void **old_out)
{
    if (t == NULL || val == NULL) {
        return -FASTIX_EINVAL;
    }
    unsigned need_h = height_needed(index);
    if (t->height < need_h) {
        int err = grow_height(t, need_h);
        if (err != 0) {
            return err;
        }
    }

    struct radix_node *path[RADIX_MAX_HEIGHT];
    unsigned slots[RADIX_MAX_HEIGHT];
    unsigned top = t->height - 1;
    struct radix_node *node = t->root;

    for (unsigned lvl = top; lvl > 0; lvl--) {
        unsigned s = slot_at(index, lvl);
        path[lvl] = node;
        slots[lvl] = s;
        if (node->slots[s] == NULL) {
            struct radix_node *child = slab_alloc(&t->node_cache);
            if (child == NULL) {
                prune_path(t, path, slots, top, lvl);
                return -FASTIX_ENOMEM;
            }
            node->slots[s] = child;
            node->count++;
        }
        node = node->slots[s];
    }

    unsigned leaf_s = slot_at(index, 0);
    void *prev = node->slots[leaf_s];
    if (old_out != NULL) {
        *old_out = prev;
    }
    node->slots[leaf_s] = val;
    if (prev == NULL) {
        node->count++;
        t->count++;
    }
    return 0;
}

void *radix_del(struct radix_tree *t, uint64_t index)
{
    if (t == NULL || t->root == NULL || index > max_index_for_height(t->height)) {
        return NULL;
    }
    struct radix_node *path[RADIX_MAX_HEIGHT];
    unsigned slots[RADIX_MAX_HEIGHT];
    unsigned top = t->height - 1;
    struct radix_node *node = t->root;

    for (unsigned lvl = top; lvl > 0; lvl--) {
        unsigned s = slot_at(index, lvl);
        path[lvl] = node;
        slots[lvl] = s;
        node = node->slots[s];
        if (node == NULL) {
            return NULL;
        }
    }

    unsigned leaf_s = slot_at(index, 0);
    void *removed = node->slots[leaf_s];
    if (removed == NULL) {
        return NULL;
    }
    node->slots[leaf_s] = NULL;
    node->count--;
    t->count--;
    path[0] = node;
    prune_path(t, path, slots, top, 0);
    return removed;
}

void *radix_next(const struct radix_tree *t, uint64_t start_index, uint64_t *found_index)
{
    if (t == NULL || t->root == NULL || start_index > max_index_for_height(t->height)) {
        return NULL;
    }
    const struct radix_node *path[RADIX_MAX_HEIGHT];
    unsigned next_slot[RADIX_MAX_HEIGHT];
    unsigned top = t->height - 1;
    unsigned lvl = top;
    bool exact = true;
    uint64_t cur_index = 0;

    path[top] = t->root;
    next_slot[top] = slot_at(start_index, top);

    for (;;) {
        const struct radix_node *node = path[lvl];
        unsigned s = next_slot[lvl];

        while (s < RADIX_FANOUT && node->slots[s] == NULL) {
            s++;
        }
        if (s == RADIX_FANOUT) {
            if (lvl == top) {
                return NULL;
            }
            lvl++;
            exact = false;
            continue;
        }
        if (s > next_slot[lvl]) {
            exact = false;
        }
        next_slot[lvl] = s + 1;
        uint64_t shift = (uint64_t)lvl * RADIX_BITS;
        cur_index = (cur_index & ~((uint64_t)RADIX_MASK << shift)) | ((uint64_t)s << shift);

        if (lvl == 0) {
            if (found_index != NULL) {
                *found_index = cur_index;
            }
            return node->slots[s];
        }
        lvl--;
        path[lvl] = node->slots[s];
        next_slot[lvl] = exact ? slot_at(start_index, lvl) : 0;
    }
}

void radix_clear(struct radix_tree *t)
{
    if (t == NULL || t->root == NULL) {
        return;
    }
    struct radix_node *path[RADIX_MAX_HEIGHT];
    unsigned next_slot[RADIX_MAX_HEIGHT];
    unsigned top = t->height - 1;
    unsigned lvl = top;

    path[top] = t->root;
    next_slot[top] = 0;

    for (;;) {
        struct radix_node *node = path[lvl];
        if (lvl == 0 || next_slot[lvl] == RADIX_FANOUT) {
            slab_free(&t->node_cache, node);
            if (lvl == top) {
                break;
            }
            lvl++;
            continue;
        }
        unsigned s = next_slot[lvl]++;
        if (node->slots[s] != NULL) {
            lvl--;
            path[lvl] = node->slots[s];
            next_slot[lvl] = 0;
        }
    }
    t->root = NULL;
    t->height = 0;
    t->count = 0;
}
