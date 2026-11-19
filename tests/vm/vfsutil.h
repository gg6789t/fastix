/*
 * vfsutil.h - test wrappers for vfsd calls: paths and buffers are copied through this
 * thread's I/O window (as libfastix_sys will do for musl), results copied back out.
 */
#ifndef FASTIX_VFSUTIL_H
#define FASTIX_VFSUTIL_H

#include <stddef.h>
#include <stdint.h>

#include <fastix/fs_abi.h>

int64_t v_open(const char *path, uint64_t flags, uint64_t mode);
int64_t v_close(int64_t fd);
int64_t v_read(int64_t fd, void *buf, uint64_t len);
int64_t v_write(int64_t fd, const void *buf, uint64_t len);
int64_t v_pread(int64_t fd, void *buf, uint64_t len, uint64_t off);
int64_t v_pwrite(int64_t fd, const void *buf, uint64_t len, uint64_t off);
int64_t v_lseek(int64_t fd, int64_t off, int whence);
int64_t v_ftruncate(int64_t fd, uint64_t len);
int64_t v_fstat(int64_t fd, struct fastix_stat *st);
int64_t v_stat(const char *path, struct fastix_stat *st, uint64_t flags);
int64_t v_mkdir(const char *path, uint64_t mode);
int64_t v_rmdir(const char *path);
int64_t v_unlink(const char *path);
int64_t v_symlink(const char *target, const char *path);
int64_t v_readlink(const char *path, char *buf, uint64_t len);
int64_t v_link(const char *from, const char *to);
int64_t v_rename(const char *from, const char *to, uint64_t flags);
int64_t v_chdir(const char *path);
int64_t v_fchdir(int64_t fd);
int64_t v_getcwd(char *buf, uint64_t len);
int64_t v_access(const char *path, uint64_t mode);
int64_t v_chmod(const char *path, uint64_t mode);
int64_t v_getdents(int64_t fd, void *buf, uint64_t len);
int64_t v_mount_tmpfs(const char *target);
int64_t v_umount(const char *target);
int64_t v_fcntl(int64_t fd, uint64_t cmd, uint64_t arg);
int64_t v_dup(int64_t fd);
int64_t v_dup3(int64_t fd, int64_t newfd, uint64_t flags);

/* Writes `s` (with its NUL) to a file. Returns 0 or -errno. */
int64_t v_put_file(const char *path, const char *s, uint64_t mode);

/* vfsd's live objects (the call also makes vfsd finish every pending exit). */
struct vfs_counts {
    uint64_t clients, ofds, nodes, pages;
};
struct vfs_counts v_counts(void);
int v_same_counts(struct vfs_counts a, struct vfs_counts b);

int str_same(const char *a, const char *b);

#endif /* FASTIX_VFSUTIL_H */
