/* fs_path.c - vfsd calls that take a path (see fsops.h). Error choices follow Linux. */
#include <fastix/errno.h>
#include <libos/mem.h>

#include "fsops.h"
#include "tmpfs.h"
#include "vfsd.h"

#define WX (FASTIX_W_OK | FASTIX_X_OK)
#define MOUNT_TMPFS_BYTES (16ULL << 20)

/* vfsd handles one call at a time, so path buffers and walks can be static (no big stack). */
static char path1[FASTIX_PATH_MAX], path2[FASTIX_PATH_MAX];
static struct walk w1, w2;

/* Copies the path at path_u and walks it from dirfd (or cwd / root). */
static int64_t walk_at(struct client *c, int64_t dirfd, uint64_t path_u, unsigned flags,
                       char *path, struct walk *w)
{
    w->v = NULL;
    w->dir = NULL;
    int err = ustr(c->slot, path_u, path, FASTIX_PATH_MAX);
    if (err != 0) {
        return err;
    }
    struct vnode *start = c->cwd;
    if (path[0] != '/' && (int32_t)dirfd != FASTIX_AT_FDCWD) {
        const struct ofd *o = fd_get(c, (int32_t)dirfd);
        if (o == NULL) {
            return -FASTIX_EBADF;
        }
        if (o->owner != FASTIX_OWNER_VFSD || !vnode_is_dir(o->v)) {
            return -FASTIX_ENOTDIR;
        }
        start = o->v;
    }
    struct cred cr = client_cred(c);
    return vfs_walk(&cr, vfs_root(), start, path, flags, w);
}

/* Creates `name` in w->dir after the usual checks; *out gets a reference. */
static int64_t create_in(struct client *c, struct walk *w, uint32_t mode, const char *target,
                         size_t tlen, struct vnode **out)
{
    struct cred cr = client_cred(c);
    int err = vnode_access(w->dir, &cr, WX);
    if (err != 0) {
        return err;
    }
    return w->dir->ops->create(w->dir, w->name, w->len, mode, &cr, target, tlen, out);
}

/* ---- open ---- */

/* O_CREAT: finds or creates the file, following a final symlink (even a dangling one) unless
 * O_EXCL or O_NOFOLLOW. *created tells the caller to skip permission checks. */
static int64_t open_creat(struct client *c, int64_t dirfd, uint64_t path_u, uint64_t flags,
                          uint64_t mode, struct vnode **out, bool *created)
{
    int64_t err = walk_at(c, dirfd, path_u, WALK_PARENT, path1, &w1);
    for (unsigned links = 0; err == 0; links++) {
        if (w1.len == 0 || name_is_dot(w1.name, w1.len) || (w1.v != NULL && vnode_is_dir(w1.v))) {
            err = (flags & FASTIX_O_EXCL) ? -FASTIX_EEXIST : -FASTIX_EISDIR;
            break;
        }
        if (w1.v == NULL) {
            uint32_t m = FASTIX_S_IFREG | ((uint32_t)mode & FASTIX_S_IALLUGO & ~client_umask(c));
            err = w1.slash ? -FASTIX_EISDIR : create_in(c, &w1, m, NULL, 0, out);
            *created = err == 0;
            break;
        }
        if (flags & FASTIX_O_EXCL) {
            err = -FASTIX_EEXIST;
            break;
        }
        if (!vnode_is_lnk(w1.v) || (flags & FASTIX_O_NOFOLLOW)) {
            vnode_get(w1.v);
            *out = w1.v;
            break;
        }
        if (links >= FASTIX_SYMLOOP_MAX) {
            err = -FASTIX_ELOOP;
            break;
        }
        size_t tlen = 0; /* walk the symlink target from the link's directory */
        (void)w1.v->ops->readlink(w1.v, path2, sizeof(path2) - 1, &tlen);
        path2[tlen] = '\0';
        struct vnode *dir = w1.dir;
        vnode_get(dir);
        walk_done(&w1);
        struct cred cr = client_cred(c);
        err = tlen == 0 ? -FASTIX_ENOENT
                        : vfs_walk(&cr, vfs_root(), dir, path2, WALK_PARENT, &w1);
        vnode_put(dir);
    }
    walk_done(&w1);
    return err;
}

static int64_t open_check(struct client *c, struct vnode *v, uint64_t flags, bool created)
{
    uint32_t acc = (uint32_t)flags & FASTIX_O_ACCMODE;
    if (vnode_is_lnk(v)) {
        return -FASTIX_ELOOP; /* O_NOFOLLOW on a symlink */
    }
    if ((flags & FASTIX_O_DIRECTORY) && !vnode_is_dir(v)) {
        return -FASTIX_ENOTDIR;
    }
    if (vnode_is_dir(v) && (acc != FASTIX_O_RDONLY || (flags & FASTIX_O_TRUNC))) {
        return -FASTIX_EISDIR;
    }
    if (created) {
        return 0;
    }
    uint32_t want = 0;
    if (acc != FASTIX_O_WRONLY) {
        want |= FASTIX_R_OK;
    }
    if (acc != FASTIX_O_RDONLY || (flags & FASTIX_O_TRUNC)) {
        want |= FASTIX_W_OK;
    }
    struct cred cr = client_cred(c);
    return vnode_access(v, &cr, want);
}

int64_t fs_openat(struct client *c, const uint64_t a[6])
{
    uint64_t flags = a[2];
    if ((flags & FASTIX_O_ACCMODE) == FASTIX_O_ACCMODE) {
        return -FASTIX_EINVAL;
    }
    struct vnode *v = NULL;
    bool created = false;
    int64_t err;
    if (flags & FASTIX_O_CREAT) {
        err = open_creat(c, (int64_t)a[0], a[1], flags, a[3], &v, &created);
    } else {
        unsigned wf = (flags & FASTIX_O_NOFOLLOW) ? 0u : WALK_FOLLOW;
        err = walk_at(c, (int64_t)a[0], a[1], wf, path1, &w1);
        if (err == 0) {
            v = w1.v;
            w1.v = NULL;
        }
        walk_done(&w1);
    }
    if (err == 0) {
        err = open_check(c, v, flags, created);
    }
    if (err == 0 && (flags & FASTIX_O_TRUNC) && vnode_is_reg(v) && v->size != 0) {
        err = v->ops->truncate(v, 0);
    }
    struct ofd *o = err == 0 ? ofd_new_vnode(v, (uint32_t)flags) : NULL;
    if (err == 0 && o == NULL) {
        err = -FASTIX_ENOMEM;
    }
    if (err != 0) {
        if (v != NULL) {
            vnode_put(v);
        }
        return err;
    }
    int fd = fd_install(c, o, 0, (flags & FASTIX_O_CLOEXEC) != 0);
    if (fd < 0) {
        ofd_put(o);
    }
    return fd;
}

/* ---- create & remove names ---- */

int64_t fs_mkdirat(struct client *c, const uint64_t a[6])
{
    int64_t err = walk_at(c, (int64_t)a[0], a[1], WALK_PARENT, path1, &w1);
    if (err == 0 && (w1.len == 0 || w1.v != NULL || name_is_dot(w1.name, w1.len))) {
        err = -FASTIX_EEXIST;
    }
    if (err == 0) {
        uint32_t m = FASTIX_S_IFDIR | ((uint32_t)a[2] & 01777u & ~client_umask(c));
        struct vnode *v;
        err = create_in(c, &w1, m, NULL, 0, &v);
        if (err == 0) {
            vnode_put(v);
        }
    }
    walk_done(&w1);
    return err;
}

/* Checks for unlink (dir == false) or rmdir (dir == true) of w1's last component. */
static int64_t remove_check(struct client *c, bool dir)
{
    if (w1.len == 0) {
        return dir ? -FASTIX_EBUSY : -FASTIX_EISDIR;
    }
    if (name_is_dot(w1.name, w1.len)) {
        return !dir ? -FASTIX_EISDIR : (w1.len == 1 ? -FASTIX_EINVAL : -FASTIX_ENOTEMPTY);
    }
    if (w1.v == NULL) {
        return -FASTIX_ENOENT;
    }
    if (dir && !vnode_is_dir(w1.v)) {
        return -FASTIX_ENOTDIR;
    }
    if (!dir && vnode_is_dir(w1.v)) {
        return -FASTIX_EISDIR;
    }
    if (!dir && w1.slash) {
        return -FASTIX_ENOTDIR;
    }
    if (dir && (w1.v->covered != NULL || w1.v == w1.v->mnt->root)) {
        return -FASTIX_EBUSY;
    }
    struct cred cr = client_cred(c);
    int err = vnode_access(w1.dir, &cr, WX);
    if (err == 0) {
        err = vnode_may_delete(w1.dir, w1.v, &cr);
    }
    if (err == 0 && dir && !w1.v->ops->dir_empty(w1.v)) {
        err = -FASTIX_ENOTEMPTY;
    }
    return err;
}

int64_t fs_unlinkat(struct client *c, const uint64_t a[6])
{
    if ((a[2] & ~(uint64_t)FASTIX_AT_REMOVEDIR) != 0) {
        return -FASTIX_EINVAL;
    }
    int64_t err = walk_at(c, (int64_t)a[0], a[1], WALK_PARENT, path1, &w1);
    if (err == 0) {
        err = remove_check(c, (a[2] & FASTIX_AT_REMOVEDIR) != 0);
    }
    if (err == 0) {
        err = w1.dir->ops->unlink(w1.dir, w1.name, w1.len);
    }
    walk_done(&w1);
    return err;
}

int64_t fs_symlinkat(struct client *c, const uint64_t a[6])
{
    int64_t err = ustr(c->slot, a[0], path2, sizeof(path2));
    if (err == 0 && path2[0] == '\0') {
        err = -FASTIX_ENOENT;
    }
    if (err == 0) {
        err = walk_at(c, (int64_t)a[1], a[2], WALK_PARENT, path1, &w1);
    }
    if (err == 0 && (w1.len == 0 || w1.v != NULL || name_is_dot(w1.name, w1.len))) {
        err = -FASTIX_EEXIST;
    }
    if (err == 0 && w1.slash) {
        err = -FASTIX_ENOENT;
    }
    if (err == 0) {
        struct vnode *v;
        size_t tlen = 0;
        while (path2[tlen] != '\0') {
            tlen++;
        }
        err = create_in(c, &w1, FASTIX_S_IFLNK | 0777u, path2, tlen, &v);
        if (err == 0) {
            vnode_put(v);
        }
    }
    walk_done(&w1);
    return err;
}

int64_t fs_readlinkat(struct client *c, const uint64_t a[6])
{
    if ((int64_t)a[3] <= 0) {
        return -FASTIX_EINVAL;
    }
    int64_t err = walk_at(c, (int64_t)a[0], a[1], 0, path1, &w1);
    if (err == 0 && !vnode_is_lnk(w1.v)) {
        err = -FASTIX_EINVAL;
    }
    if (err == 0) {
        char *buf = uptr(c->slot, a[2], a[3]);
        size_t n = 0;
        err = buf == NULL ? -FASTIX_EFAULT : w1.v->ops->readlink(w1.v, buf, a[3], &n);
        if (err == 0) {
            err = (int64_t)n;
        }
    }
    walk_done(&w1);
    return err;
}

int64_t fs_linkat(struct client *c, const uint64_t a[6])
{
    uint64_t flags = a[4];
    if ((flags & ~(uint64_t)FASTIX_AT_SYMLINK_FOLLOW) != 0) {
        return -FASTIX_EINVAL;
    }
    unsigned wf = (flags & FASTIX_AT_SYMLINK_FOLLOW) ? WALK_FOLLOW : 0u;
    int64_t err = walk_at(c, (int64_t)a[0], a[1], wf, path2, &w2);
    if (err == 0 && vnode_is_dir(w2.v)) {
        err = -FASTIX_EPERM;
    }
    if (err == 0) {
        err = walk_at(c, (int64_t)a[2], a[3], WALK_PARENT, path1, &w1);
    }
    if (err == 0 && (w1.len == 0 || w1.v != NULL || name_is_dot(w1.name, w1.len))) {
        err = -FASTIX_EEXIST;
    }
    if (err == 0 && w1.slash) {
        err = -FASTIX_ENOENT;
    }
    if (err == 0 && w1.dir->mnt != w2.v->mnt) {
        err = -FASTIX_EXDEV;
    }
    if (err == 0) {
        struct cred cr = client_cred(c);
        err = vnode_access(w1.dir, &cr, WX);
    }
    if (err == 0) {
        err = w1.dir->ops->link(w1.dir, w1.name, w1.len, w2.v);
    }
    walk_done(&w1);
    walk_done(&w2);
    return err;
}

/* ---- rename ---- */

/* True if directory `a` is `d` or one of d's ancestors (same filesystem). */
static bool is_ancestor(struct vnode *a, struct vnode *d)
{
    struct vnode *cur = d;
    vnode_get(cur);
    bool found = false;
    while (!found && cur != NULL) {
        found = cur == a;
        struct vnode *p = cur == cur->mnt->root ? NULL : cur->ops->parent(cur);
        vnode_put(cur);
        cur = p;
    }
    if (cur != NULL) {
        vnode_put(cur);
    }
    return found;
}

static int64_t rename_check(struct client *c, uint64_t flags)
{
    struct vnode *src = w1.v, *dst = w2.v;
    if (w1.len == 0 || w2.len == 0 || name_is_dot(w1.name, w1.len) ||
        name_is_dot(w2.name, w2.len)) {
        return -FASTIX_EBUSY;
    }
    if (src == NULL) {
        return -FASTIX_ENOENT;
    }
    if (w1.dir->mnt != w2.dir->mnt) {
        return -FASTIX_EXDEV;
    }
    if (!vnode_is_dir(src) && (w1.slash || w2.slash)) {
        return -FASTIX_ENOTDIR;
    }
    if (src->covered != NULL || (dst != NULL && dst->covered != NULL)) {
        return -FASTIX_EBUSY;
    }
    if (dst != NULL && (flags & FASTIX_RENAME_NOREPLACE)) {
        return -FASTIX_EEXIST;
    }
    if (dst != NULL && dst != src) {
        if (vnode_is_dir(src) && !vnode_is_dir(dst)) {
            return -FASTIX_ENOTDIR;
        }
        if (!vnode_is_dir(src) && vnode_is_dir(dst)) {
            return -FASTIX_EISDIR;
        }
        if (vnode_is_dir(dst) && !dst->ops->dir_empty(dst)) {
            return -FASTIX_ENOTEMPTY;
        }
    }
    if (vnode_is_dir(src) && is_ancestor(src, w2.dir)) {
        return -FASTIX_EINVAL; /* a directory cannot move into itself */
    }
    struct cred cr = client_cred(c);
    int err = vnode_access(w1.dir, &cr, WX);
    if (err == 0) {
        err = vnode_access(w2.dir, &cr, WX);
    }
    if (err == 0) {
        err = vnode_may_delete(w1.dir, src, &cr);
    }
    if (err == 0 && dst != NULL) {
        err = vnode_may_delete(w2.dir, dst, &cr);
    }
    if (err == 0 && vnode_is_dir(src) && w1.dir != w2.dir) {
        err = vnode_access(src, &cr, FASTIX_W_OK); /* its ".." changes */
    }
    return err;
}

int64_t fs_renameat2(struct client *c, const uint64_t a[6])
{
    uint64_t flags = a[4];
    if ((flags & ~(uint64_t)FASTIX_RENAME_NOREPLACE) != 0) {
        return -FASTIX_EINVAL;
    }
    int64_t err = walk_at(c, (int64_t)a[0], a[1], WALK_PARENT, path1, &w1);
    if (err == 0) {
        err = walk_at(c, (int64_t)a[2], a[3], WALK_PARENT, path2, &w2);
    }
    if (err == 0) {
        err = rename_check(c, flags);
    }
    if (err == 0 && w1.v != w2.v) {
        err = w1.dir->ops->rename(w1.dir, w1.name, w1.len, w2.dir, w2.name, w2.len);
    }
    walk_done(&w1);
    walk_done(&w2);
    return err;
}

/* ---- working directory ---- */

static int64_t set_cwd(struct client *c, struct vnode *v)
{
    if (!vnode_is_dir(v)) {
        return -FASTIX_ENOTDIR;
    }
    struct cred cr = client_cred(c);
    int err = vnode_access(v, &cr, FASTIX_X_OK);
    if (err != 0) {
        return err;
    }
    vnode_get(v);
    vnode_put(c->cwd);
    c->cwd = v;
    return 0;
}

int64_t fs_chdir(struct client *c, const uint64_t a[6])
{
    int64_t err = walk_at(c, FASTIX_AT_FDCWD, a[0], WALK_FOLLOW, path1, &w1);
    if (err == 0) {
        err = set_cwd(c, w1.v);
    }
    walk_done(&w1);
    return err;
}

int64_t fs_fchdir(struct client *c, const uint64_t a[6])
{
    const struct ofd *o = fd_get(c, (int64_t)a[0]);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    return o->owner == FASTIX_OWNER_VFSD ? set_cwd(c, o->v) : -FASTIX_ENOTDIR;
}

/* Builds the cwd path backwards at the end of path1. Returns its start or NULL (-ENOENT). */
static char *cwd_path(const struct client *c, size_t *len)
{
    char *end = path1 + sizeof(path1) - 1, *p = end;
    *end = '\0';
    struct vnode *cur = c->cwd;
    while (cur != vfs_root()) {
        if (cur == cur->mnt->root && cur->mnt->mountpoint != NULL) {
            cur = cur->mnt->mountpoint;
            continue;
        }
        const char *name;
        size_t n;
        struct vnode *parent = cur->ops->parent(cur);
        if (parent == NULL || cur->ops->dir_name(cur, &name, &n) != 0 ||
            (size_t)(p - path1) < n + 1) {
            if (parent != NULL) {
                vnode_put(parent);
            }
            return NULL;
        }
        p -= n;
        memcpy(p, name, n);
        *--p = '/';
        vnode_put(parent); /* still linked into the tree, so cur's parent stays alive */
        cur = parent;
    }
    if (p == end) {
        *--p = '/';
    }
    *len = (size_t)(end - p);
    return p;
}

int64_t fs_getcwd(struct client *c, const uint64_t a[6])
{
    size_t len = 0;
    const char *p = cwd_path(c, &len);
    if (p == NULL) {
        return -FASTIX_ENOENT;
    }
    if (a[1] < len + 1) {
        return -FASTIX_ERANGE;
    }
    char *buf = uptr(c->slot, a[0], len + 1);
    if (buf == NULL) {
        return -FASTIX_EFAULT;
    }
    memcpy(buf, p, len + 1);
    return (int64_t)(len + 1);
}

/* ---- attributes ---- */

int64_t fs_truncate(struct client *c, const uint64_t a[6])
{
    if ((int64_t)a[1] < 0) {
        return -FASTIX_EINVAL;
    }
    int64_t err = walk_at(c, FASTIX_AT_FDCWD, a[0], WALK_FOLLOW, path1, &w1);
    if (err == 0) {
        struct cred cr = client_cred(c);
        err = vnode_is_dir(w1.v)    ? -FASTIX_EISDIR
              : !vnode_is_reg(w1.v) ? -FASTIX_EINVAL
                                    : vnode_access(w1.v, &cr, FASTIX_W_OK);
    }
    if (err == 0) {
        err = w1.v->ops->truncate(w1.v, a[1]);
    }
    walk_done(&w1);
    return err;
}

int64_t fs_faccessat(struct client *c, const uint64_t a[6])
{
    uint64_t mode = a[2], flags = a[3];
    if ((mode & ~7ull) != 0 ||
        (flags & ~(uint64_t)(FASTIX_AT_EACCESS | FASTIX_AT_SYMLINK_NOFOLLOW)) != 0) {
        return -FASTIX_EINVAL;
    }
    unsigned wf = (flags & FASTIX_AT_SYMLINK_NOFOLLOW) ? 0u : WALK_FOLLOW;
    int64_t err = walk_at(c, (int64_t)a[0], a[1], wf, path1, &w1);
    if (err == 0 && mode != FASTIX_F_OK) {
        struct cred cr = (flags & FASTIX_AT_EACCESS) ? client_cred(c) : client_real_cred(c);
        err = vnode_access(w1.v, &cr, (uint32_t)mode);
    }
    walk_done(&w1);
    return err;
}

int64_t fs_fchmodat(struct client *c, const uint64_t a[6])
{
    if (a[3] != 0) {
        return -FASTIX_EINVAL;
    }
    int64_t err = walk_at(c, (int64_t)a[0], a[1], WALK_FOLLOW, path1, &w1);
    if (err == 0) {
        struct cred cr = client_cred(c);
        err = fs_chmod_vnode(w1.v, &cr, a[2]);
    }
    walk_done(&w1);
    return err;
}

int64_t fs_newfstatat(struct client *c, const uint64_t a[6])
{
    uint64_t flags = a[3];
    if ((flags & ~(uint64_t)(FASTIX_AT_SYMLINK_NOFOLLOW | FASTIX_AT_EMPTY_PATH)) != 0) {
        return -FASTIX_EINVAL;
    }
    struct fastix_stat *st = uptr(c->slot, a[2], sizeof(*st));
    if (st == NULL) {
        return -FASTIX_EFAULT;
    }
    const char *first = uptr(c->slot, a[1], 1);
    if ((flags & FASTIX_AT_EMPTY_PATH) && first != NULL && *first == '\0') {
        if ((int32_t)a[0] == FASTIX_AT_FDCWD) {
            vnode_stat(c->cwd, st);
            return 0;
        }
        const struct ofd *o = fd_get(c, (int64_t)a[0]);
        if (o == NULL) {
            return -FASTIX_EBADF;
        }
        fs_stat_ofd(o, st);
        return 0;
    }
    unsigned wf = (flags & FASTIX_AT_SYMLINK_NOFOLLOW) ? 0u : WALK_FOLLOW;
    int64_t err = walk_at(c, (int64_t)a[0], a[1], wf, path1, &w1);
    if (err == 0) {
        vnode_stat(w1.v, st);
    }
    walk_done(&w1);
    return err;
}

/* ---- mounts (tmpfs only for now; root only) ---- */

int64_t fs_mount(struct client *c, const uint64_t a[6])
{
    struct cred cr = client_cred(c);
    if (cr.uid != 0) {
        return -FASTIX_EPERM;
    }
    int64_t err = ustr(c->slot, a[2], path2, sizeof(path2));
    if (err == 0 && memcmp(path2, "tmpfs", 6) != 0) {
        err = -FASTIX_ENODEV;
    }
    if (err == 0) {
        err = walk_at(c, FASTIX_AT_FDCWD, a[1], WALK_FOLLOW, path1, &w1);
    }
    if (err == 0 && w1.v == vfs_root()) {
        err = -FASTIX_EBUSY;
    }
    struct vnode *root = NULL;
    if (err == 0) {
        err = tmpfs_new(MOUNT_TMPFS_BYTES, 01777u, &cr, &root);
    }
    if (err == 0) {
        err = vfs_mount(w1.v, root);
        if (err != 0) {
            root->ops->destroy(root);
        }
    }
    walk_done(&w1);
    return err;
}

int64_t fs_umount2(struct client *c, const uint64_t a[6])
{
    struct cred cr = client_cred(c);
    if (cr.uid != 0) {
        return -FASTIX_EPERM;
    }
    if (a[1] != 0) {
        return -FASTIX_EINVAL;
    }
    int64_t err = walk_at(c, FASTIX_AT_FDCWD, a[0], WALK_FOLLOW, path1, &w1);
    if (err == 0) {
        err = vfs_umount(w1.v);
        if (err == 0) {
            w1.v = NULL; /* freed together with the filesystem */
        }
    }
    walk_done(&w1);
    return err;
}
