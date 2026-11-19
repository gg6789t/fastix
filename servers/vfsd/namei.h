/*
 * namei.h - path walk: turns a path into a vnode (or a parent directory and a last name),
 * handling ".", "..", mounts, symlinks (ELOOP after FASTIX_SYMLOOP_MAX), trailing slashes
 * and search permission. Host-testable.
 */
#ifndef VFSD_NAMEI_H
#define VFSD_NAMEI_H

#include <stdbool.h>
#include <stddef.h>

#include <fastix/config.h>
#include <libos/compiler.h>

#include "vnode.h"

#define WALK_FOLLOW 0x1u /* follow a symlink in the last component */
#define WALK_PARENT 0x2u /* stop at the parent directory of the last component */

struct walk {
    struct vnode *dir;  /* WALK_PARENT: parent directory (reference) */
    struct vnode *v;    /* result (reference); WALK_PARENT: last component if it exists */
    const char *name;   /* WALK_PARENT: last component (inside buf); len 0 for "/" */
    size_t len;
    bool slash;         /* the path ended with '/' */
    char buf[FASTIX_PATH_MAX];
    char link[FASTIX_PATH_MAX];
};

/* Walks `path` from `start` (or from `root` if it begins with '/'). On success w->v and
 * w->dir hold references that walk_done drops. With WALK_PARENT and a last component of
 * "." or "..", w->v is NULL and the caller decides. Returns 0 or -errno. */
LIBOS_MUST_CHECK int vfs_walk(const struct cred *cr, struct vnode *root, struct vnode *start,
                              const char *path, unsigned flags, struct walk *w);

void walk_done(struct walk *w);

/* True if (name, len) is "." or "..". */
bool name_is_dot(const char *name, size_t len);

#endif /* VFSD_NAMEI_H */
