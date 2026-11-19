/* fs_file.c - vfsd calls on open FDs (see fsops.h). */
#include <fastix/errno.h>
#include <fastix/vfs_proto.h>
#include <libos/mem.h>

#include "fsops.h"
#include "vfsd.h"

#define DIRENT_HDR 19u /* offsetof(struct fastix_dirent64, d_name) */

/* The vnode behind a vfsd-owned FD, or NULL with *err set. */
static struct vnode *fd_vnode(const struct client *c, int64_t fd, struct ofd **o, int64_t *err)
{
    *o = fd_get(c, fd);
    if (*o == NULL) {
        *err = -FASTIX_EBADF;
        return NULL;
    }
    if ((*o)->owner != FASTIX_OWNER_VFSD) {
        *err = -FASTIX_ESPIPE;
        return NULL;
    }
    return (*o)->v;
}

static int64_t do_read(struct ofd *o, struct vnode *v, uint32_t slot, uint64_t buf_u,
                       uint64_t len, uint64_t off)
{
    if ((o->flags & FASTIX_O_ACCMODE) == FASTIX_O_WRONLY) {
        return -FASTIX_EBADF;
    }
    if (vnode_is_dir(v)) {
        return -FASTIX_EISDIR;
    }
    if (len == 0) {
        return 0;
    }
    void *dst = uptr(slot, buf_u, len);
    return dst != NULL ? v->ops->read(v, off, dst, len) : -FASTIX_EFAULT;
}

static int64_t do_write(struct ofd *o, struct vnode *v, uint32_t slot, uint64_t buf_u,
                        uint64_t len, uint64_t off)
{
    if ((o->flags & FASTIX_O_ACCMODE) == FASTIX_O_RDONLY) {
        return -FASTIX_EBADF;
    }
    if (len == 0) {
        return 0;
    }
    const void *src = uptr(slot, buf_u, len);
    return src != NULL ? v->ops->write(v, off, src, len) : -FASTIX_EFAULT;
}

int64_t fs_read(struct client *c, const uint64_t a[6], uint32_t *owner)
{
    struct ofd *o = fd_get(c, (int64_t)a[0]);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    if (o->owner != FASTIX_OWNER_VFSD) {
        *owner = o->owner;
        return -FASTIX_ESTALEFD;
    }
    int64_t n = do_read(o, o->v, c->slot, a[1], a[2], o->pos);
    if (n > 0) {
        o->pos += (uint64_t)n;
    }
    return n;
}

int64_t fs_write(struct client *c, const uint64_t a[6], uint32_t *owner)
{
    struct ofd *o = fd_get(c, (int64_t)a[0]);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    if (o->owner != FASTIX_OWNER_VFSD) {
        *owner = o->owner;
        return -FASTIX_ESTALEFD;
    }
    if (o->flags & FASTIX_O_APPEND) {
        o->pos = (uint64_t)o->v->size; /* atomic: vfsd handles one call at a time */
    }
    int64_t n = do_write(o, o->v, c->slot, a[1], a[2], o->pos);
    if (n > 0) {
        o->pos += (uint64_t)n;
    }
    return n;
}

int64_t fs_pread64(struct client *c, const uint64_t a[6])
{
    struct ofd *o;
    int64_t err;
    struct vnode *v = fd_vnode(c, (int64_t)a[0], &o, &err);
    if (v == NULL) {
        return err;
    }
    return (int64_t)a[3] < 0 ? -FASTIX_EINVAL : do_read(o, v, c->slot, a[1], a[2], a[3]);
}

int64_t fs_pwrite64(struct client *c, const uint64_t a[6])
{
    struct ofd *o;
    int64_t err;
    struct vnode *v = fd_vnode(c, (int64_t)a[0], &o, &err);
    if (v == NULL) {
        return err;
    }
    return (int64_t)a[3] < 0 ? -FASTIX_EINVAL : do_write(o, v, c->slot, a[1], a[2], a[3]);
}

int64_t fs_lseek(struct client *c, const uint64_t a[6])
{
    struct ofd *o;
    int64_t err;
    struct vnode *v = fd_vnode(c, (int64_t)a[0], &o, &err);
    if (v == NULL) {
        return err;
    }
    int64_t off = (int64_t)a[1], base;
    switch (a[2]) {
    case FASTIX_SEEK_SET:
        base = 0;
        break;
    case FASTIX_SEEK_CUR:
        base = (int64_t)o->pos;
        break;
    case FASTIX_SEEK_END:
        base = v->size;
        break;
    default:
        return -FASTIX_EINVAL;
    }
    if ((off > 0 && base > INT64_MAX - off)) {
        return -FASTIX_EOVERFLOW;
    }
    if (base + off < 0) {
        return -FASTIX_EINVAL;
    }
    o->pos = (uint64_t)(base + off);
    return (int64_t)o->pos;
}

void fs_stat_ofd(const struct ofd *o, struct fastix_stat *st)
{
    if (o->owner == FASTIX_OWNER_VFSD) {
        vnode_stat(o->v, st);
        return;
    }
    memset(st, 0, sizeof(*st));
    st->st_ino = o->obj;
    st->st_mode = 0600u | (o->kind == FASTIX_OBJ_PIPE ? FASTIX_S_IFIFO : 0u);
    st->st_nlink = 1;
    st->st_blksize = FASTIX_PAGE_SIZE;
}

int64_t fs_fstat(struct client *c, const uint64_t a[6])
{
    const struct ofd *o = fd_get(c, (int64_t)a[0]);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    struct fastix_stat *st = uptr(c->slot, a[1], sizeof(*st));
    if (st == NULL) {
        return -FASTIX_EFAULT;
    }
    fs_stat_ofd(o, st);
    return 0;
}

int64_t fs_getdents64(struct client *c, const uint64_t a[6])
{
    struct ofd *o = fd_get(c, (int64_t)a[0]);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    if (o->owner != FASTIX_OWNER_VFSD || !vnode_is_dir(o->v)) {
        return -FASTIX_ENOTDIR;
    }
    uint8_t *buf = uptr(c->slot, a[1], a[2]);
    if (buf == NULL) {
        return -FASTIX_EFAULT;
    }
    uint64_t used = 0;
    struct dir_entry e;
    while (o->v->ops->readdir(o->v, o->pos, &e) == 1) {
        uint64_t reclen = (DIRENT_HDR + e.len + 1u + 7u) & ~7ull;
        if (used + reclen > a[2]) {
            if (used == 0) {
                return -FASTIX_EINVAL; /* buffer too small for even one entry */
            }
            break;
        }
        struct fastix_dirent64 *d = (struct fastix_dirent64 *)(void *)(buf + used);
        memset(d, 0, (size_t)reclen);
        d->d_ino = e.ino;
        d->d_off = (int64_t)e.cookie;
        d->d_reclen = (uint16_t)reclen;
        d->d_type = e.type;
        memcpy(d->d_name, e.name, e.len);
        used += reclen;
        o->pos = e.cookie;
    }
    return (int64_t)used;
}

int64_t fs_ftruncate(struct client *c, const uint64_t a[6])
{
    struct ofd *o = fd_get(c, (int64_t)a[0]);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    if ((int64_t)a[1] < 0 || o->owner != FASTIX_OWNER_VFSD || !vnode_is_reg(o->v) ||
        (o->flags & FASTIX_O_ACCMODE) == FASTIX_O_RDONLY) {
        return -FASTIX_EINVAL;
    }
    return o->v->ops->truncate(o->v, a[1]);
}

int64_t fs_chmod_vnode(struct vnode *v, const struct cred *cr, uint64_t mode)
{
    if (cr->uid != 0 && cr->uid != v->uid) {
        return -FASTIX_EPERM;
    }
    uint32_t m = (uint32_t)mode & FASTIX_S_IALLUGO;
    if (cr->uid != 0 && cr->gid != v->gid) {
        m &= ~FASTIX_S_ISGID; /* only group members may set set-group-ID */
    }
    v->mode = (v->mode & FASTIX_S_IFMT) | m;
    vfs_now(&v->ctime);
    return 0;
}

int64_t fs_fchmod(struct client *c, const uint64_t a[6])
{
    struct ofd *o = fd_get(c, (int64_t)a[0]);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    if (o->owner != FASTIX_OWNER_VFSD) {
        return -FASTIX_EINVAL;
    }
    struct cred cr = client_cred(c);
    return fs_chmod_vnode(o->v, &cr, a[1]);
}
