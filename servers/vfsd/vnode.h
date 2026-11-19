/*
 * vnode.h - the VFS core of vfsd: in-memory file objects (vnodes), the operations a
 * filesystem provides, mounts, credentials and permission checks. Host-testable: no seL4.
 */
#ifndef VFSD_VNODE_H
#define VFSD_VNODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <fastix/config.h>
#include <fastix/fs_abi.h>
#include <fastix/time_abi.h>
#include <libos/compiler.h>
#include <libos/slab.h>

struct vnode;
struct mount;

/* Who is asking (from the client's procinfo page). */
struct cred {
    uint32_t uid; /* effective */
    uint32_t gid; /* effective */
};

/* One directory entry produced by readdir. */
struct dir_entry {
    uint64_t ino;
    uint64_t cookie; /* pass back to continue after this entry */
    uint8_t type;    /* FASTIX_DT_* */
    const char *name;
    size_t len;
};

/* Operations a filesystem provides. Names are not NUL-terminated (name, len); `len` is
 * 1..FASTIX_NAME_MAX and never "." or "..". Lookups return a new reference. */
struct vnode_ops {
    int (*lookup)(struct vnode *dir, const char *name, size_t len, struct vnode **out);
    /* Creates a file, directory or symlink (mode has the type bits). target is for symlinks. */
    int (*create)(struct vnode *dir, const char *name, size_t len, uint32_t mode,
                  const struct cred *cr, const char *target, size_t tlen, struct vnode **out);
    int (*link)(struct vnode *dir, const char *name, size_t len, struct vnode *v);
    /* Removes an entry; the caller already checked file vs. empty directory. */
    int (*unlink)(struct vnode *dir, const char *name, size_t len);
    /* Moves an entry, replacing any existing target (already checked by the caller). */
    int (*rename)(struct vnode *odir, const char *oname, size_t olen, struct vnode *ndir,
                  const char *nname, size_t nlen);
    /* ".." of a directory (new reference); a filesystem root returns itself. */
    struct vnode *(*parent)(struct vnode *dir);
    /* Name of directory `dir` inside its parent; -ENOENT once it was removed. */
    int (*dir_name)(struct vnode *dir, const char **name, size_t *len);
    bool (*dir_empty)(struct vnode *dir);
    int64_t (*read)(struct vnode *v, uint64_t off, void *buf, size_t len);
    int64_t (*write)(struct vnode *v, uint64_t off, const void *buf, size_t len);
    int (*truncate)(struct vnode *v, uint64_t size);
    int (*readlink)(struct vnode *v, char *buf, size_t cap, size_t *len);
    /* Next entry after `cookie` (0 = start, "." and ".." first): 1 found, 0 at the end. */
    int (*readdir)(struct vnode *dir, uint64_t cookie, struct dir_entry *out);
    void (*free)(struct vnode *v); /* last reference gone and no links left */
    void (*destroy)(struct vnode *root); /* frees a whole unmounted filesystem */
};

struct vnode {
    uint64_t ino;
    uint32_t mode;  /* FASTIX_S_IF* | permission bits */
    uint32_t nlink; /* names that point to it */
    uint32_t refs;  /* in-memory references (open files, cwds, walks, the mount root) */
    uint32_t uid, gid;
    int64_t size;
    struct fastix_timespec atime, mtime, ctime;
    struct mount *mnt;     /* filesystem it belongs to */
    struct mount *covered; /* mount whose root is mounted on this directory, or NULL */
    const struct vnode_ops *ops;
};

struct mount {
    uint32_t id;
    uint32_t refs;            /* in-memory references to all of its vnodes */
    struct vnode *root;       /* reference held while mounted */
    struct vnode *mountpoint; /* directory it covers (reference), NULL for "/" */
    struct mount *parent;
};

static inline bool vnode_is_dir(const struct vnode *v)
{
    return (v->mode & FASTIX_S_IFMT) == FASTIX_S_IFDIR;
}

static inline bool vnode_is_lnk(const struct vnode *v)
{
    return (v->mode & FASTIX_S_IFMT) == FASTIX_S_IFLNK;
}

static inline bool vnode_is_reg(const struct vnode *v)
{
    return (v->mode & FASTIX_S_IFMT) == FASTIX_S_IFREG;
}

void vnode_get(struct vnode *v);
void vnode_put(struct vnode *v); /* frees it once refs and nlink are both 0 */

/* Permission check: mask of FASTIX_R_OK | W_OK | X_OK. Returns 0 or -EACCES. */
int vnode_access(const struct vnode *v, const struct cred *cr, uint32_t mask);

/* Sticky-directory rule for removing/renaming `v` out of `dir`: 0 or -EPERM. */
int vnode_may_delete(const struct vnode *dir, const struct vnode *v, const struct cred *cr);

/* Fills a struct stat for v (st_dev = mount id). */
void vnode_stat(const struct vnode *v, struct fastix_stat *st);

/* Current wall-clock time (vfsd: VDSO; host tests: a counter). */
void vfs_now(struct fastix_timespec *ts);

/* Page source for every slab cache in vfsd (vfsd: mmap; host tests: malloc). */
struct slab_provider vfs_page_provider(void);

/* Mount table. */
#define VFS_MAX_MOUNTS 16

/* Makes `root` the root filesystem ("/"); takes the caller's reference. */
LIBOS_MUST_CHECK int vfs_mount_root(struct vnode *root, struct mount **out);

/* Mounts `root` on directory `on` (reference to `on` taken by the mount). */
LIBOS_MUST_CHECK int vfs_mount(struct vnode *on, struct vnode *root);

/* Unmounts the filesystem whose root is `root` and frees it. The caller passes its own
 * reference to `root`; -EBUSY if anything else in that filesystem is still in use. */
LIBOS_MUST_CHECK int vfs_umount(struct vnode *root);

#endif /* VFSD_VNODE_H */
