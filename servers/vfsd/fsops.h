/*
 * fsops.h - vfsd's file system calls. Each takes the calling client and the six call
 * arguments (Linux order; pointers point into the client's I/O window) and returns a result
 * or -errno. fs_path.c: calls that take a path; fs_file.c: calls on an open FD.
 */
#ifndef VFSD_FSOPS_H
#define VFSD_FSOPS_H

#include <stdint.h>

#include "client.h"
#include "namei.h"

/* fs_path.c */
int64_t fs_openat(struct client *c, const uint64_t a[6]);
int64_t fs_mkdirat(struct client *c, const uint64_t a[6]);
int64_t fs_unlinkat(struct client *c, const uint64_t a[6]);
int64_t fs_symlinkat(struct client *c, const uint64_t a[6]);
int64_t fs_readlinkat(struct client *c, const uint64_t a[6]);
int64_t fs_linkat(struct client *c, const uint64_t a[6]);
int64_t fs_renameat2(struct client *c, const uint64_t a[6]);
int64_t fs_chdir(struct client *c, const uint64_t a[6]);
int64_t fs_fchdir(struct client *c, const uint64_t a[6]);
int64_t fs_getcwd(struct client *c, const uint64_t a[6]);
int64_t fs_truncate(struct client *c, const uint64_t a[6]);
int64_t fs_faccessat(struct client *c, const uint64_t a[6]);
int64_t fs_fchmodat(struct client *c, const uint64_t a[6]);
int64_t fs_newfstatat(struct client *c, const uint64_t a[6]);
int64_t fs_mount(struct client *c, const uint64_t a[6]);
int64_t fs_umount2(struct client *c, const uint64_t a[6]);

/* fs_file.c; *owner is set when the FD belongs to another server (-FASTIX_ESTALEFD). */
int64_t fs_read(struct client *c, const uint64_t a[6], uint32_t *owner);
int64_t fs_write(struct client *c, const uint64_t a[6], uint32_t *owner);
int64_t fs_pread64(struct client *c, const uint64_t a[6]);
int64_t fs_pwrite64(struct client *c, const uint64_t a[6]);
int64_t fs_lseek(struct client *c, const uint64_t a[6]);
int64_t fs_fstat(struct client *c, const uint64_t a[6]);
int64_t fs_getdents64(struct client *c, const uint64_t a[6]);
int64_t fs_ftruncate(struct client *c, const uint64_t a[6]);
int64_t fs_fchmod(struct client *c, const uint64_t a[6]);

/* Shared by both files: stat of an open file, chmod of a vnode. */
void fs_stat_ofd(const struct ofd *o, struct fastix_stat *st);
int64_t fs_chmod_vnode(struct vnode *v, const struct cred *cr, uint64_t mode);

#endif /* VFSD_FSOPS_H */
