/* vnode.c - vnode references, permissions, stat and the mount table (see vnode.h). */
#include <fastix/errno.h>
#include <libos/mem.h>

#include "vnode.h"

static struct mount mounts[VFS_MAX_MOUNTS];

void vnode_get(struct vnode *v)
{
    v->refs++;
    v->mnt->refs++;
}

void vnode_put(struct vnode *v)
{
    v->mnt->refs--;
    if (--v->refs == 0 && v->nlink == 0) {
        v->ops->free(v);
    }
}

int vnode_access(const struct vnode *v, const struct cred *cr, uint32_t mask)
{
    uint32_t mode = v->mode;
    if (cr->uid == 0) {
        /* root may read and write anything; it may execute only if someone may, or a dir */
        bool any_x = (mode & 0111u) != 0 || vnode_is_dir(v);
        return (mask & FASTIX_X_OK) && !any_x ? -FASTIX_EACCES : 0;
    }
    uint32_t bits = mode & 07u;
    if (cr->uid == v->uid) {
        bits = (mode >> 6) & 07u;
    } else if (cr->gid == v->gid) {
        bits = (mode >> 3) & 07u;
    }
    return (bits & mask) == mask ? 0 : -FASTIX_EACCES;
}

int vnode_may_delete(const struct vnode *dir, const struct vnode *v, const struct cred *cr)
{
    if (!(dir->mode & FASTIX_S_ISVTX) || cr->uid == 0 || cr->uid == v->uid ||
        cr->uid == dir->uid) {
        return 0;
    }
    return -FASTIX_EPERM;
}

void vnode_stat(const struct vnode *v, struct fastix_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_dev = v->mnt->id;
    st->st_ino = v->ino;
    st->st_mode = v->mode;
    st->st_nlink = v->nlink;
    st->st_uid = v->uid;
    st->st_gid = v->gid;
    st->st_size = v->size;
    st->st_blksize = FASTIX_PAGE_SIZE;
    st->st_blocks = (v->size + 511) / 512;
    st->st_atim = v->atime;
    st->st_mtim = v->mtime;
    st->st_ctim = v->ctime;
}

static struct mount *mount_slot(void)
{
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].root == NULL) {
            mounts[i] = (struct mount){ .id = i + 1 };
            return &mounts[i];
        }
    }
    return NULL;
}

int vfs_mount_root(struct vnode *root, struct mount **out)
{
    struct mount *m = mount_slot();
    if (m == NULL) {
        return -FASTIX_ENOMEM;
    }
    m->root = root;
    root->mnt = m;
    m->refs = root->refs;
    *out = m;
    return 0;
}

int vfs_mount(struct vnode *on, struct vnode *root)
{
    if (!vnode_is_dir(on)) {
        return -FASTIX_ENOTDIR;
    }
    if (on->covered != NULL) {
        return -FASTIX_EBUSY;
    }
    struct mount *m = mount_slot();
    if (m == NULL) {
        return -FASTIX_ENOMEM;
    }
    m->root = root;
    root->mnt = m;
    m->refs = root->refs;
    m->mountpoint = on;
    m->parent = on->mnt;
    vnode_get(on);
    on->covered = m;
    return 0;
}

int vfs_umount(struct vnode *root)
{
    struct mount *m = root->mnt;
    if (m->root != root || m->mountpoint == NULL) {
        return -FASTIX_EINVAL; /* not a mounted root, or the root filesystem */
    }
    if (m->refs != 2) {
        return -FASTIX_EBUSY; /* only the mount and the caller may hold references */
    }
    m->mountpoint->covered = NULL;
    vnode_put(m->mountpoint);
    root->refs = 0;
    m->refs = 0;
    root->ops->destroy(root);
    *m = (struct mount){ 0 };
    return 0;
}
