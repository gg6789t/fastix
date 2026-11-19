/*
 * rtree.c - AVL tree of non-overlapping ranges (see libos/rtree.h).
 * Recursion depth is bounded by the tree height (about 1.44 * log2(count)).
 */
#include <stddef.h>

#include <fastix/errno.h>
#include <libos/assert.h>
#include <libos/rtree.h>

static int height(const struct rnode *n)
{
    return n == NULL ? 0 : n->height;
}

static void update(struct rnode *n)
{
    int l = height(n->left), r = height(n->right);
    n->height = 1 + (l > r ? l : r);
}

static struct rnode *rotate_right(struct rnode *n)
{
    struct rnode *l = n->left;
    n->left = l->right;
    l->right = n;
    update(n);
    update(l);
    return l;
}

static struct rnode *rotate_left(struct rnode *n)
{
    struct rnode *r = n->right;
    n->right = r->left;
    r->left = n;
    update(n);
    update(r);
    return r;
}

/* Restores the AVL property at n after one of its subtrees changed height by 1. */
static struct rnode *balance(struct rnode *n)
{
    update(n);
    int diff = height(n->left) - height(n->right);
    if (diff > 1) {
        if (height(n->left->left) < height(n->left->right)) {
            n->left = rotate_left(n->left);
        }
        return rotate_right(n);
    }
    if (diff < -1) {
        if (height(n->right->right) < height(n->right->left)) {
            n->right = rotate_right(n->right);
        }
        return rotate_left(n);
    }
    return n;
}

static struct rnode *node_insert(struct rnode *h, struct rnode *n)
{
    if (h == NULL) {
        return n;
    }
    if (n->start < h->start) {
        h->left = node_insert(h->left, n);
    } else {
        h->right = node_insert(h->right, n);
    }
    return balance(h);
}

static struct rnode *remove_min(struct rnode *h, struct rnode **min)
{
    if (h->left == NULL) {
        *min = h;
        return h->right;
    }
    h->left = remove_min(h->left, min);
    return balance(h);
}

static struct rnode *node_remove(struct rnode *h, const struct rnode *n)
{
    libos_assert(h != NULL); /* n must be in the tree */
    if (n->start < h->start) {
        h->left = node_remove(h->left, n);
    } else if (n->start > h->start) {
        h->right = node_remove(h->right, n);
    } else {
        libos_assert(h == n);
        struct rnode *l = h->left, *r = h->right, *m;
        if (r == NULL) {
            return l;
        }
        r = remove_min(r, &m);
        m->left = l;
        m->right = r;
        return balance(m);
    }
    return balance(h);
}

void rtree_init(struct rtree *t)
{
    t->root = NULL;
    t->count = 0;
}

int rtree_insert(struct rtree *t, struct rnode *n)
{
    if (n->start >= n->end) {
        return -FASTIX_EINVAL;
    }
    const struct rnode *next = rtree_next(t, n->start);
    if (next != NULL && next->start < n->end) {
        return -FASTIX_EEXIST;
    }
    n->left = NULL;
    n->right = NULL;
    n->height = 1;
    t->root = node_insert(t->root, n);
    t->count++;
    return 0;
}

void rtree_remove(struct rtree *t, struct rnode *n)
{
    t->root = node_remove(t->root, n);
    t->count--;
}

struct rnode *rtree_find(const struct rtree *t, uint64_t addr)
{
    struct rnode *n = rtree_next(t, addr);
    return (n != NULL && n->start <= addr) ? n : NULL;
}

struct rnode *rtree_next(const struct rtree *t, uint64_t addr)
{
    struct rnode *n = t->root, *best = NULL;

    /* Ranges are disjoint, so "ends after addr" is monotonic in start order. */
    while (n != NULL) {
        if (n->end > addr) {
            best = n;
            n = n->left;
        } else {
            n = n->right;
        }
    }
    return best;
}
