/* namei.c - path walk (see namei.h). No recursion: symlinks are spliced into the buffer. */
#include <fastix/errno.h>
#include <libos/mem.h>

#include "namei.h"

bool name_is_dot(const char *name, size_t len)
{
    return (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.');
}

void walk_done(struct walk *w)
{
    if (w->v != NULL) {
        vnode_put(w->v);
        w->v = NULL;
    }
    if (w->dir != NULL) {
        vnode_put(w->dir);
        w->dir = NULL;
    }
}

/* Moves down into whatever is mounted on v (consumes the reference to v). */
static struct vnode *cross_down(struct vnode *v)
{
    while (v->covered != NULL) {
        struct vnode *r = v->covered->root;
        vnode_get(r);
        vnode_put(v);
        v = r;
    }
    return v;
}

/* ".." of cur, never above `root` and across mount boundaries. New reference or NULL. */
static struct vnode *dotdot(struct vnode *cur, struct vnode *root)
{
    while (cur != root && cur == cur->mnt->root && cur->mnt->mountpoint != NULL) {
        cur = cur->mnt->mountpoint; /* a mount root's ".." is its mountpoint's ".." */
    }
    if (cur == root) {
        vnode_get(root);
        return root;
    }
    return cur->ops->parent(cur);
}

static size_t str_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

/* Replaces the walked part of w->buf with the symlink target: buf = target + rest. */
static int splice_link(struct walk *w, struct vnode *lnk, const char *rest)
{
    size_t tlen = 0;
    int err = lnk->ops->readlink(lnk, w->link, sizeof(w->link), &tlen);
    if (err != 0) {
        return err;
    }
    if (tlen == 0) {
        return -FASTIX_ENOENT;
    }
    size_t rlen = str_len(rest, sizeof(w->buf));
    if (tlen + rlen + 1 > sizeof(w->buf)) {
        return -FASTIX_ENAMETOOLONG;
    }
    memmove(w->buf + tlen, rest, rlen + 1);
    memcpy(w->buf, w->link, tlen);
    return 0;
}

/* WALK_PARENT: records the parent and the last name, and looks the name up if it exists. */
static int finish_parent(struct walk *w, struct vnode *cur, const char *comp, size_t len,
                         bool slash)
{
    w->dir = cur;
    w->name = comp;
    w->len = len;
    w->slash = slash;
    if (name_is_dot(comp, len)) {
        return 0;
    }
    int err = cur->ops->lookup(cur, comp, len, &w->v);
    if (err == -FASTIX_ENOENT) {
        w->v = NULL;
        return 0;
    }
    return err;
}

/* Resolves one component of directory cur into a new reference in *next. */
static int step(struct vnode *cur, struct vnode *root, const char *comp, size_t len,
                struct vnode **next)
{
    if (len == 1 && comp[0] == '.') {
        vnode_get(cur);
        *next = cur;
        return 0;
    }
    if (len == 2 && comp[0] == '.' && comp[1] == '.') {
        *next = dotdot(cur, root);
        return *next != NULL ? 0 : -FASTIX_ENOENT;
    }
    int err = cur->ops->lookup(cur, comp, len, next);
    if (err == 0) {
        *next = cross_down(*next);
    }
    return err;
}

int vfs_walk(const struct cred *cr, struct vnode *root, struct vnode *start, const char *path,
             unsigned flags, struct walk *w)
{
    w->dir = NULL;
    w->v = NULL;
    w->slash = false;
    size_t plen = str_len(path, sizeof(w->buf));
    if (plen == 0) {
        return -FASTIX_ENOENT;
    }
    if (plen >= sizeof(w->buf)) {
        return -FASTIX_ENAMETOOLONG;
    }
    memcpy(w->buf, path, plen + 1);
    char *rest = w->buf;
    struct vnode *cur = rest[0] == '/' ? root : start;
    vnode_get(cur);
    unsigned links = 0;
    int err = 0;

    for (;;) {
        bool abs = rest[0] == '/';
        while (*rest == '/') {
            rest++;
        }
        if (*rest == '\0') { /* "/" or a symlink to "/": the result is cur itself */
            w->v = cur;
            if (flags & WALK_PARENT) { /* no last name: report the directory with len 0 */
                vnode_get(cur);
                w->dir = cur;
                w->name = rest;
                w->len = 0;
                w->slash = abs;
            }
            return 0;
        }
        char *comp = rest;
        size_t len = 0;
        while (comp[len] != '\0' && comp[len] != '/') {
            len++;
        }
        char *after = comp + len;
        char *tail = after;
        while (*tail == '/') {
            tail++;
        }
        bool last = *tail == '\0';
        bool slash = last && after != tail;
        if (len > FASTIX_NAME_MAX) {
            err = -FASTIX_ENAMETOOLONG;
            break;
        }
        if (!vnode_is_dir(cur)) {
            err = -FASTIX_ENOTDIR;
            break;
        }
        if ((err = vnode_access(cur, cr, FASTIX_X_OK)) != 0) {
            break;
        }
        if (last && (flags & WALK_PARENT)) {
            err = finish_parent(w, cur, comp, len, slash);
            if (err != 0) {
                w->dir = NULL;
                break;
            }
            return 0;
        }
        struct vnode *next;
        if ((err = step(cur, root, comp, len, &next)) != 0) {
            break;
        }
        if (vnode_is_lnk(next) && (!last || slash || (flags & WALK_FOLLOW))) {
            if (++links > FASTIX_SYMLOOP_MAX) {
                vnode_put(next);
                err = -FASTIX_ELOOP;
                break;
            }
            err = splice_link(w, next, after);
            vnode_put(next);
            if (err != 0) {
                break;
            }
            rest = w->buf;
            if (rest[0] == '/') {
                vnode_put(cur);
                cur = root;
                vnode_get(cur);
            }
            continue;
        }
        vnode_put(cur);
        cur = next;
        if (last) {
            if (slash && !vnode_is_dir(cur)) {
                vnode_put(cur);
                return -FASTIX_ENOTDIR;
            }
            w->v = cur;
            return 0;
        }
        rest = tail;
    }
    vnode_put(cur);
    return err;
}
