/* vfsutil.c - vfsd call wrappers for the QEMU tests (see vfsutil.h). */
#include <fastix/abi.h>
#include <fastix/errno.h>
#include <fastix_sys/sys.h>
#include <libos/kprint.h>
#include <libos/mem.h>

#include "vfsutil.h"

#define OFF_P1   0u      /* first path */
#define OFF_P2   4096u   /* second path */
#define OFF_DATA 8192u   /* data, stat buffers, ... */
#define FDCWD    ((uint64_t)(int64_t)FASTIX_AT_FDCWD)

static char *win(size_t off)
{
    return (char *)fastix_iowin() + off;
}

static uint64_t at(size_t off)
{
    return (uint64_t)(uintptr_t)win(off);
}

static uint64_t put(size_t off, const char *s)
{
    char *d = win(off);
    size_t i = 0;
    do {
        d[i] = s[i];
    } while (s[i++] != '\0');
    return at(off);
}

int str_same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int64_t v_open(const char *path, uint64_t flags, uint64_t mode)
{
    return fastix_vfs4(FASTIX_SYS_openat, FDCWD, put(OFF_P1, path), flags, mode);
}

int64_t v_close(int64_t fd)
{
    return fastix_vfs1(FASTIX_SYS_close, (uint64_t)fd);
}

int64_t v_read(int64_t fd, void *buf, uint64_t len)
{
    return fastix_fd_read((int32_t)fd, buf, len);
}

int64_t v_write(int64_t fd, const void *buf, uint64_t len)
{
    return fastix_fd_write((int32_t)fd, buf, len);
}

int64_t v_pread(int64_t fd, void *buf, uint64_t len, uint64_t off)
{
    int64_t r = fastix_vfs4(FASTIX_SYS_pread64, (uint64_t)fd, at(OFF_DATA), len, off);
    if (r > 0) {
        memcpy(buf, win(OFF_DATA), (size_t)r);
    }
    return r;
}

int64_t v_pwrite(int64_t fd, const void *buf, uint64_t len, uint64_t off)
{
    memcpy(win(OFF_DATA), buf, (size_t)len);
    return fastix_vfs4(FASTIX_SYS_pwrite64, (uint64_t)fd, at(OFF_DATA), len, off);
}

int64_t v_lseek(int64_t fd, int64_t off, int whence)
{
    return fastix_vfs3(FASTIX_SYS_lseek, (uint64_t)fd, (uint64_t)off, (uint64_t)whence);
}

int64_t v_ftruncate(int64_t fd, uint64_t len)
{
    return fastix_vfs2(FASTIX_SYS_ftruncate, (uint64_t)fd, len);
}

int64_t v_fstat(int64_t fd, struct fastix_stat *st)
{
    int64_t r = fastix_vfs2(FASTIX_SYS_fstat, (uint64_t)fd, at(OFF_DATA));
    if (r == 0) {
        memcpy(st, win(OFF_DATA), sizeof(*st));
    }
    return r;
}

int64_t v_stat(const char *path, struct fastix_stat *st, uint64_t flags)
{
    int64_t r = fastix_vfs4(FASTIX_SYS_newfstatat, FDCWD, put(OFF_P1, path), at(OFF_DATA), flags);
    if (r == 0) {
        memcpy(st, win(OFF_DATA), sizeof(*st));
    }
    return r;
}

int64_t v_mkdir(const char *path, uint64_t mode)
{
    return fastix_vfs3(FASTIX_SYS_mkdirat, FDCWD, put(OFF_P1, path), mode);
}

int64_t v_rmdir(const char *path)
{
    return fastix_vfs3(FASTIX_SYS_unlinkat, FDCWD, put(OFF_P1, path), FASTIX_AT_REMOVEDIR);
}

int64_t v_unlink(const char *path)
{
    return fastix_vfs3(FASTIX_SYS_unlinkat, FDCWD, put(OFF_P1, path), 0);
}

int64_t v_symlink(const char *target, const char *path)
{
    return fastix_vfs3(FASTIX_SYS_symlinkat, put(OFF_P2, target), FDCWD, put(OFF_P1, path));
}

int64_t v_readlink(const char *path, char *buf, uint64_t len)
{
    int64_t r = fastix_vfs4(FASTIX_SYS_readlinkat, FDCWD, put(OFF_P1, path), at(OFF_DATA), len);
    if (r > 0) {
        memcpy(buf, win(OFF_DATA), (size_t)r);
    }
    return r;
}

int64_t v_link(const char *from, const char *to)
{
    return fastix_vfs5(FASTIX_SYS_linkat, FDCWD, put(OFF_P2, from), FDCWD, put(OFF_P1, to), 0);
}

int64_t v_rename(const char *from, const char *to, uint64_t flags)
{
    return fastix_vfs5(FASTIX_SYS_renameat2, FDCWD, put(OFF_P2, from), FDCWD, put(OFF_P1, to),
                       flags);
}

int64_t v_chdir(const char *path)
{
    return fastix_vfs1(FASTIX_SYS_chdir, put(OFF_P1, path));
}

int64_t v_fchdir(int64_t fd)
{
    return fastix_vfs1(FASTIX_SYS_fchdir, (uint64_t)fd);
}

int64_t v_getcwd(char *buf, uint64_t len)
{
    int64_t r = fastix_vfs2(FASTIX_SYS_getcwd, at(OFF_DATA), len);
    if (r > 0) {
        memcpy(buf, win(OFF_DATA), (size_t)r);
    }
    return r;
}

int64_t v_access(const char *path, uint64_t mode)
{
    return fastix_vfs4(FASTIX_SYS_faccessat, FDCWD, put(OFF_P1, path), mode, 0);
}

int64_t v_chmod(const char *path, uint64_t mode)
{
    return fastix_vfs4(FASTIX_SYS_fchmodat, FDCWD, put(OFF_P1, path), mode, 0);
}

int64_t v_getdents(int64_t fd, void *buf, uint64_t len)
{
    int64_t r = fastix_vfs3(FASTIX_SYS_getdents64, (uint64_t)fd, at(OFF_DATA), len);
    if (r > 0) {
        memcpy(buf, win(OFF_DATA), (size_t)r);
    }
    return r;
}

int64_t v_mount_tmpfs(const char *target)
{
    return fastix_vfs5(FASTIX_SYS_mount, put(OFF_P2, "none"), put(OFF_P1, target),
                       put(OFF_DATA, "tmpfs"), 0, 0);
}

int64_t v_umount(const char *target)
{
    return fastix_vfs2(FASTIX_SYS_umount2, put(OFF_P1, target), 0);
}

int64_t v_fcntl(int64_t fd, uint64_t cmd, uint64_t arg)
{
    return fastix_vfs3(FASTIX_SYS_fcntl, (uint64_t)fd, cmd, arg);
}

int64_t v_dup(int64_t fd)
{
    return fastix_vfs1(FASTIX_SYS_dup, (uint64_t)fd);
}

int64_t v_dup3(int64_t fd, int64_t newfd, uint64_t flags)
{
    return fastix_vfs3(FASTIX_SYS_dup3, (uint64_t)fd, (uint64_t)newfd, flags);
}

int64_t v_put_file(const char *path, const char *s, uint64_t mode)
{
    int64_t fd = v_open(path, FASTIX_O_CREAT | FASTIX_O_WRONLY | FASTIX_O_TRUNC, mode);
    if (fd < 0) {
        return fd;
    }
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    int64_t w = v_write(fd, s, n + 1);
    int64_t c = v_close(fd);
    return w < 0 ? w : c;
}

struct vfs_counts v_counts(void)
{
    uint64_t more[FASTIX_CALL_MORE];
    (void)fastix_vfs(FASTIX_VFS_TEST_STATS, 0, NULL, more);
    return (struct vfs_counts){ more[0], more[1], more[2], more[3] };
}

int v_same_counts(struct vfs_counts a, struct vfs_counts b)
{
    if (a.clients == b.clients && a.ofds == b.ofds && a.nodes == b.nodes && a.pages == b.pages) {
        return 1;
    }
    kprint_str("  vfsd counts differ (clients ofds nodes pages): ");
    kprint_u64(a.clients);
    kprint_str(" ");
    kprint_u64(a.ofds);
    kprint_str(" ");
    kprint_u64(a.nodes);
    kprint_str(" ");
    kprint_u64(a.pages);
    kprint_str(" -> ");
    kprint_u64(b.clients);
    kprint_str(" ");
    kprint_u64(b.ofds);
    kprint_str(" ");
    kprint_u64(b.nodes);
    kprint_str(" ");
    kprint_u64(b.pages);
    kprint_str("\n");
    return 0;
}
