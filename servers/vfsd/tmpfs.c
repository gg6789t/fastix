/* tmpfs.c - in-memory filesystem (see tmpfs.h). */
#include <fastix/errno.h>
#include <libos/list.h>
#include <libos/mem.h>
#include <libos/radix.h>

#include "tmpfs.h"

#define PAGE FASTIX_PAGE_SIZE
#define FIRST_COOKIE 3u /* 1 = ".", 2 = ".." */

struct tmpfs {
    uint64_t next_ino;
    uint64_t max_pages;
    uint64_t pages;
};

struct tnode;

struct tdirent {
    struct list_node link; /* in the directory, oldest first */
    uint64_t cookie;       /* readdir position, grows with every new entry */
    struct tnode *node;
    uint8_t len;
    char name[FASTIX_NAME_MAX];
};

struct tnode {
    struct vnode v;
    struct tmpfs *fs;
    struct tnode *parent;     /* directories: parent (root: itself; NULL once removed) */
    struct tdirent *self;     /* directories: own entry in the parent (NULL for the root) */
    struct list_node entries; /* directories */
    uint64_t next_cookie;     /* directories */
    struct radix_tree pages;  /* regular files: page index -> data page */
    char *target;             /* symlinks */
};

static struct slab_cache fs_cache, node_cache, dirent_cache, page_cache, radix_nodes;
static struct slab_cache link_small, link_mid, link_big; /* symlink targets by size */
static const struct vnode_ops tmpfs_ops;

void tmpfs_module_init(void)
{
    struct slab_provider pp = vfs_page_provider();
    slab_init(&fs_cache, "tmpfs", sizeof(struct tmpfs), pp);
    slab_init(&node_cache, "tmpfs_node", sizeof(struct tnode), pp);
    slab_init(&dirent_cache, "tmpfs_dirent", sizeof(struct tdirent), pp);
    slab_init(&page_cache, "tmpfs_page", PAGE, pp);
    slab_init(&radix_nodes, "tmpfs_radix", radix_node_size(), pp);
    slab_init(&link_small, "tmpfs_link64", 64, pp);
    slab_init(&link_mid, "tmpfs_link512", 512, pp);
    slab_init(&link_big, "tmpfs_link4k", FASTIX_PATH_MAX, pp);
}

static struct slab_cache *link_cache(size_t len)
{
    return len <= 64 ? &link_small : len <= 512 ? &link_mid : &link_big;
}

struct tmpfs_stats tmpfs_stats(void)
{
    return (struct tmpfs_stats){ node_cache.in_use, page_cache.in_use };
}

static struct tnode *T(struct vnode *v)
{
    return (struct tnode *)(void *)v; /* v is the first member */
}

static void touch(struct vnode *v, bool mtime)
{
    vfs_now(&v->ctime);
    if (mtime) {
        v->mtime = v->ctime;
    }
}

static struct tdirent *find(struct tnode *dir, const char *name, size_t len)
{
    struct list_node *n;
    LIST_FOREACH(n, &dir->entries) {
        struct tdirent *d = LIST_ENTRY(n, struct tdirent, link);
        if (d->len == len && memcmp(d->name, name, len) == 0) {
            return d;
        }
    }
    return NULL;
}

static struct tnode *node_new(struct tmpfs *fs, uint32_t mode, uint32_t uid, uint32_t gid)
{
    struct tnode *t = slab_alloc(&node_cache);
    if (t == NULL) {
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->fs = fs;
    t->v.ino = fs->next_ino++;
    t->v.mode = mode;
    t->v.uid = uid;
    t->v.gid = gid;
    t->v.refs = 1;
    t->v.ops = &tmpfs_ops;
    vfs_now(&t->v.atime);
    t->v.mtime = t->v.atime;
    t->v.ctime = t->v.atime;
    if (vnode_is_dir(&t->v)) {
        t->v.nlink = 2;
        list_init(&t->entries);
        t->next_cookie = FIRST_COOKIE;
    } else {
        t->v.nlink = 1;
    }
    radix_init(&t->pages, &radix_nodes);
    return t;
}

static void add_entry(struct tnode *dir, struct tdirent *d, const char *name, size_t len,
                      struct tnode *node)
{
    d->len = (uint8_t)len;
    memcpy(d->name, name, len);
    d->node = node;
    d->cookie = dir->next_cookie++;
    list_push_back(&dir->entries, &d->link);
    if (vnode_is_dir(&node->v)) {
        node->parent = dir;
        node->self = d;
        dir->v.nlink++; /* the child's ".." */
    }
    touch(&dir->v, true);
}

int tmpfs_new(uint64_t max_bytes, uint32_t mode, const struct cred *cr, struct vnode **root)
{
    struct tmpfs *fs = slab_alloc(&fs_cache);
    if (fs == NULL) {
        return -FASTIX_ENOMEM;
    }
    *fs = (struct tmpfs){ .next_ino = 1, .max_pages = max_bytes / PAGE };
    struct tnode *t = node_new(fs, FASTIX_S_IFDIR | (mode & FASTIX_S_IALLUGO), cr->uid, cr->gid);
    if (t == NULL) {
        slab_free(&fs_cache, fs);
        return -FASTIX_ENOMEM;
    }
    t->parent = t;
    *root = &t->v;
    return 0;
}

/* ---- names ---- */

static int t_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    struct tdirent *d = find(T(dir), name, len);
    if (d == NULL) {
        return -FASTIX_ENOENT;
    }
    vnode_get(&d->node->v);
    *out = &d->node->v;
    return 0;
}

static int t_create(struct vnode *dir, const char *name, size_t len, uint32_t mode,
                    const struct cred *cr, const char *target, size_t tlen, struct vnode **out)
{
    struct tnode *td = T(dir);
    if (find(td, name, len) != NULL) {
        return -FASTIX_EEXIST;
    }
    bool sgid_dir = (dir->mode & FASTIX_S_ISGID) != 0;
    uint32_t gid = sgid_dir ? dir->gid : cr->gid;
    if (sgid_dir && (mode & FASTIX_S_IFMT) == FASTIX_S_IFDIR) {
        mode |= FASTIX_S_ISGID; /* new directories inherit set-group-ID */
    }
    struct tdirent *d = slab_alloc(&dirent_cache);
    struct tnode *t = d != NULL ? node_new(td->fs, mode, cr->uid, gid) : NULL;
    if (t != NULL && (mode & FASTIX_S_IFMT) == FASTIX_S_IFLNK) {
        t->target = slab_alloc(link_cache(tlen));
        if (t->target != NULL) {
            memcpy(t->target, target, tlen);
            t->v.size = (int64_t)tlen;
        }
    }
    if (t == NULL || ((mode & FASTIX_S_IFMT) == FASTIX_S_IFLNK && t->target == NULL)) {
        if (t != NULL) {
            slab_free(&node_cache, t);
        }
        if (d != NULL) {
            slab_free(&dirent_cache, d);
        }
        return -FASTIX_ENOSPC;
    }
    t->v.mnt = dir->mnt;
    dir->mnt->refs++; /* the reference returned in *out */
    add_entry(td, d, name, len, t);
    *out = &t->v;
    return 0;
}

static int t_link(struct vnode *dir, const char *name, size_t len, struct vnode *v)
{
    if (find(T(dir), name, len) != NULL) {
        return -FASTIX_EEXIST;
    }
    struct tdirent *d = slab_alloc(&dirent_cache);
    if (d == NULL) {
        return -FASTIX_ENOSPC;
    }
    add_entry(T(dir), d, name, len, T(v));
    v->nlink++;
    touch(v, false);
    return 0;
}

/* Takes entry d out of dir and drops the link it held. */
static void drop_entry(struct tnode *dir, struct tdirent *d)
{
    struct tnode *t = d->node;
    list_remove(&d->link);
    slab_free(&dirent_cache, d);
    if (vnode_is_dir(&t->v)) {
        dir->v.nlink--;
        t->parent = NULL;
        t->self = NULL;
        t->v.nlink = 0;
    } else {
        t->v.nlink--;
    }
    touch(&t->v, false);
    touch(&dir->v, true);
    if (t->v.nlink == 0 && t->v.refs == 0) {
        t->v.ops->free(&t->v);
    }
}

static int t_unlink(struct vnode *dir, const char *name, size_t len)
{
    struct tdirent *d = find(T(dir), name, len);
    if (d == NULL) {
        return -FASTIX_ENOENT;
    }
    drop_entry(T(dir), d);
    return 0;
}

static int t_rename(struct vnode *odir, const char *oname, size_t olen, struct vnode *ndir,
                    const char *nname, size_t nlen)
{
    struct tnode *od = T(odir), *nd = T(ndir);
    struct tdirent *d = find(od, oname, olen);
    if (d == NULL) {
        return -FASTIX_ENOENT;
    }
    struct tdirent *victim = find(nd, nname, nlen);
    if (victim == d) {
        return 0;
    }
    if (victim != NULL) {
        drop_entry(nd, victim);
    }
    struct tnode *t = d->node;
    list_remove(&d->link);
    if (vnode_is_dir(&t->v)) {
        od->v.nlink--;
    }
    touch(&od->v, true);
    add_entry(nd, d, nname, nlen, t);
    touch(&t->v, false);
    return 0;
}

static struct vnode *t_parent(struct vnode *dir)
{
    struct tnode *p = T(dir)->parent;
    if (p == NULL) {
        return NULL;
    }
    vnode_get(&p->v);
    return &p->v;
}

static int t_dir_name(struct vnode *dir, const char **name, size_t *len)
{
    struct tdirent *d = T(dir)->self;
    if (d == NULL) {
        return -FASTIX_ENOENT;
    }
    *name = d->name;
    *len = d->len;
    return 0;
}

static bool t_dir_empty(struct vnode *dir)
{
    return list_empty(&T(dir)->entries);
}

static int t_readdir(struct vnode *dir, uint64_t cookie, struct dir_entry *out)
{
    struct tnode *t = T(dir);
    if (cookie < FIRST_COOKIE - 1u) {
        struct tnode *p = cookie == 0 ? t : (t->parent != NULL ? t->parent : t);
        *out = (struct dir_entry){ .ino = p->v.ino, .cookie = cookie + 1, .type = FASTIX_DT_DIR,
                                   .name = "..", .len = cookie == 0 ? 1u : 2u };
        return 1;
    }
    struct list_node *n;
    LIST_FOREACH(n, &t->entries) {
        struct tdirent *d = LIST_ENTRY(n, struct tdirent, link);
        if (d->cookie > cookie) {
            uint32_t fmt = d->node->v.mode & FASTIX_S_IFMT;
            uint8_t type = fmt == FASTIX_S_IFDIR   ? FASTIX_DT_DIR
                           : fmt == FASTIX_S_IFLNK ? FASTIX_DT_LNK
                                                   : FASTIX_DT_REG;
            *out = (struct dir_entry){ .ino = d->node->v.ino, .cookie = d->cookie, .type = type,
                                       .name = d->name, .len = d->len };
            return 1;
        }
    }
    return 0;
}

/* ---- file data ---- */

static int64_t t_read(struct vnode *v, uint64_t off, void *buf, size_t len)
{
    if (off >= (uint64_t)v->size) {
        return 0;
    }
    uint64_t end = off + len < (uint64_t)v->size ? off + len : (uint64_t)v->size;
    uint8_t *dst = buf;
    for (uint64_t pos = off; pos < end;) {
        size_t in = (size_t)(pos % PAGE);
        size_t n = PAGE - in < end - pos ? PAGE - in : (size_t)(end - pos);
        const uint8_t *page = radix_get(&T(v)->pages, pos / PAGE);
        if (page != NULL) {
            memcpy(dst, page + in, n);
        } else {
            memset(dst, 0, n); /* a hole */
        }
        dst += n;
        pos += n;
    }
    return (int64_t)(end - off);
}

static uint8_t *page_for_write(struct tnode *t, uint64_t index)
{
    uint8_t *page = radix_get(&t->pages, index);
    if (page != NULL || t->fs->pages >= t->fs->max_pages) {
        return page;
    }
    page = slab_alloc(&page_cache);
    if (page == NULL) {
        return NULL;
    }
    memset(page, 0, PAGE);
    if (radix_set(&t->pages, index, page, NULL) != 0) {
        slab_free(&page_cache, page);
        return NULL;
    }
    t->fs->pages++;
    return page;
}

static int64_t t_write(struct vnode *v, uint64_t off, const void *buf, size_t len)
{
    if (off > TMPFS_MAX_FILE_SIZE || len > TMPFS_MAX_FILE_SIZE - off) {
        return -FASTIX_EFBIG;
    }
    const uint8_t *src = buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        size_t in = (size_t)(pos % PAGE);
        size_t n = PAGE - in < len - done ? PAGE - in : (size_t)(len - done);
        uint8_t *page = page_for_write(T(v), pos / PAGE);
        if (page == NULL) {
            break;
        }
        memcpy(page + in, src + done, n);
        done += n;
    }
    if (done == 0 && len > 0) {
        return -FASTIX_ENOSPC;
    }
    if (off + done > (uint64_t)v->size) {
        v->size = (int64_t)(off + done);
    }
    touch(v, true);
    return (int64_t)done;
}

static void free_pages_from(struct tnode *t, uint64_t first)
{
    uint64_t idx;
    void *page;
    while ((page = radix_next(&t->pages, first, &idx)) != NULL) {
        (void)radix_del(&t->pages, idx);
        slab_free(&page_cache, page);
        t->fs->pages--;
    }
}

static int t_truncate(struct vnode *v, uint64_t size)
{
    if (size > TMPFS_MAX_FILE_SIZE) {
        return -FASTIX_EFBIG;
    }
    struct tnode *t = T(v);
    if (size < (uint64_t)v->size) {
        free_pages_from(t, (size + PAGE - 1) / PAGE);
        uint8_t *last = size % PAGE != 0 ? radix_get(&t->pages, size / PAGE) : NULL;
        if (last != NULL) {
            memset(last + size % PAGE, 0, PAGE - size % PAGE); /* old bytes must read as 0 */
        }
    }
    v->size = (int64_t)size;
    touch(v, true);
    return 0;
}

static int t_readlink(struct vnode *v, char *buf, size_t cap, size_t *len)
{
    size_t n = (size_t)v->size < cap ? (size_t)v->size : cap;
    memcpy(buf, T(v)->target, n);
    *len = n;
    return 0;
}

static void t_free(struct vnode *v)
{
    struct tnode *t = T(v);
    free_pages_from(t, 0);
    if (t->target != NULL) {
        slab_free(link_cache((size_t)t->v.size), t->target);
    }
    slab_free(&node_cache, t);
}

/* Frees a whole unmounted tree without recursion (AGENTS.md 2 rule 4). */
static void t_destroy(struct vnode *root)
{
    struct tnode *rt = T(root), *t = rt;
    for (;;) {
        if (!list_empty(&t->entries)) {
            struct tdirent *d = LIST_ENTRY(t->entries.next, struct tdirent, link);
            if (vnode_is_dir(&d->node->v) && !list_empty(&d->node->entries)) {
                t = d->node; /* empty the subdirectory first */
                continue;
            }
            d->node->v.refs = 0;
            drop_entry(t, d);
            continue;
        }
        if (t == rt) {
            break;
        }
        t = t->parent;
    }
    struct tmpfs *fs = rt->fs;
    t_free(root);
    slab_free(&fs_cache, fs);
}

static const struct vnode_ops tmpfs_ops = {
    .lookup = t_lookup,
    .create = t_create,
    .link = t_link,
    .unlink = t_unlink,
    .rename = t_rename,
    .parent = t_parent,
    .dir_name = t_dir_name,
    .dir_empty = t_dir_empty,
    .read = t_read,
    .write = t_write,
    .truncate = t_truncate,
    .readlink = t_readlink,
    .readdir = t_readdir,
    .free = t_free,
    .destroy = t_destroy,
};
