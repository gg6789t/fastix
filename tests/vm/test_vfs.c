/*
 * test_vfs.c - QEMU tests for vfsd (Phase 4C) part 1: file I/O, directories, symlinks and
 * hard links on the root tmpfs. Part 2 (FDs, fork/exec, cwd, mounts, permissions, errors) is
 * in test_vfs2.c.
 */
#include <stdint.h>

#include <fastix/errno.h>
#include <fastix/fs_abi.h>
#include <fastix_sys/sys.h>
#include <libos/mem.h>

#include "vfsutil.h"
#include "vmtest.h"

#define RDWR_NEW (FASTIX_O_CREAT | FASTIX_O_RDWR)

static int read_is(int64_t fd, uint64_t off, const char *want, uint64_t len)
{
    char buf[64];
    return v_pread(fd, buf, len, off) == (int64_t)len && memcmp(buf, want, (size_t)len) == 0;
}

static int all_zero(int64_t fd, uint64_t off, uint64_t len)
{
    char buf[64];
    if (v_pread(fd, buf, len, off) != (int64_t)len) {
        return 0;
    }
    for (uint64_t i = 0; i < len; i++) {
        if (buf[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void t_file_io(void)
{
    struct vfs_counts before = v_counts();
    struct fastix_stat st;
    char buf[16];
    int ok = v_mkdir("/t1", 0755) == 0;
    int64_t fd = v_open("/t1/f", RDWR_NEW | FASTIX_O_EXCL, 0666);
    ok &= fd >= 0 && v_open("/t1/f", RDWR_NEW | FASTIX_O_EXCL, 0666) == -FASTIX_EEXIST;
    ok &= v_write(fd, "hello world", 11) == 11 && v_lseek(fd, 0, FASTIX_SEEK_CUR) == 11;
    ok &= v_lseek(fd, 0, FASTIX_SEEK_SET) == 0 && v_read(fd, buf, 5) == 5 &&
          memcmp(buf, "hello", 5) == 0;
    ok &= read_is(fd, 6, "world", 5) && v_lseek(fd, 0, FASTIX_SEEK_CUR) == 5; /* pread keeps it */
    ok &= v_pwrite(fd, "HELLO", 5, 0) == 5 && read_is(fd, 0, "HELLO world", 11);
    ok &= v_fstat(fd, &st) == 0 && st.st_size == 11 && st.st_nlink == 1 &&
          st.st_mode == (FASTIX_S_IFREG | (0666u & ~fastix_procinfo()->umask));
    /* Sparse: a hole reads as zeros. */
    ok &= v_lseek(fd, 100000, FASTIX_SEEK_SET) == 100000 && v_write(fd, "x", 1) == 1;
    ok &= v_fstat(fd, &st) == 0 && st.st_size == 100001 && all_zero(fd, 50000, 32);
    /* Shrinking drops data; growing again reads zeros, not old bytes. */
    ok &= v_ftruncate(fd, 5) == 0 && v_ftruncate(fd, 10) == 0;
    ok &= read_is(fd, 0, "HELLO", 5) && all_zero(fd, 5, 5) && v_pread(fd, buf, 16, 0) == 10;
    /* O_APPEND writes at the end whatever the offset; O_TRUNC empties. */
    int64_t ap = v_open("/t1/f", FASTIX_O_WRONLY | FASTIX_O_APPEND, 0);
    ok &= v_lseek(ap, 0, FASTIX_SEEK_SET) == 0 && v_write(ap, "AB", 2) == 2 &&
          read_is(fd, 10, "AB", 2);
    ok &= v_read(ap, buf, 1) == -FASTIX_EBADF && v_lseek(ap, -1, FASTIX_SEEK_SET) == -FASTIX_EINVAL;
    int64_t ro = v_open("/t1/f", FASTIX_O_RDONLY | FASTIX_O_TRUNC, 0);
    ok &= ro >= 0 && v_fstat(fd, &st) == 0 && st.st_size == 0;
    ok &= v_write(ro, "z", 1) == -FASTIX_EBADF && v_ftruncate(ro, 1) == -FASTIX_EINVAL;
    ok &= v_close(ap) == 0 && v_close(ro) == 0 && v_close(fd) == 0 && v_close(fd) == -FASTIX_EBADF;
    ok &= v_unlink("/t1/f") == 0 && v_rmdir("/t1") == 0;
    ok &= v_same_counts(before, v_counts());
    vmtest_report("vfs_file_rw_seek_sparse_trunc_append", ok);
}

/* Reads every entry name of `path` with buffers of `chunk` bytes into names (joined by ' '). */
static int list_dir(const char *path, uint64_t chunk, char *names, size_t cap)
{
    uint8_t buf[512];
    size_t used = 0;
    int64_t fd = v_open(path, FASTIX_O_RDONLY | FASTIX_O_DIRECTORY, 0);
    int64_t n;
    while (fd >= 0 && (n = v_getdents(fd, buf, chunk)) > 0) {
        for (int64_t off = 0; off < n;) {
            const struct fastix_dirent64 *d = (const void *)(buf + off);
            for (size_t i = 0; d->d_name[i] != '\0' && used + 2 < cap; i++) {
                names[used++] = d->d_name[i];
            }
            names[used++] = d->d_type == FASTIX_DT_DIR ? '/' : ' ';
            off += d->d_reclen;
        }
    }
    names[used] = '\0';
    return fd >= 0 && v_close(fd) == 0;
}

static void t_dirs(void)
{
    struct vfs_counts before = v_counts();
    struct fastix_stat st;
    char names[128], buf[4];
    int ok = v_mkdir("/d", 0755) == 0 && v_mkdir("/d", 0755) == -FASTIX_EEXIST;
    ok &= v_mkdir("/d/a", 0755) == 0 && v_mkdir("/d/b/", 0755) == 0 &&
          v_put_file("/d/f", "0", 0644) == 0;
    ok &= list_dir("/d", 512, names, sizeof(names)) && str_same(names, "./../a/b/f ");
    ok &= list_dir("/d", 32, names, sizeof(names)) && str_same(names, "./../a/b/f ");
    int64_t dfd = v_open("/d", FASTIX_O_RDONLY, 0);
    ok &= v_getdents(dfd, buf, 4) == -FASTIX_EINVAL && v_close(dfd) == 0; /* buffer too small */
    ok &= v_rmdir("/d") == -FASTIX_ENOTEMPTY && v_rmdir("/d/f") == -FASTIX_ENOTDIR;
    ok &= v_unlink("/d/a") == -FASTIX_EISDIR && v_open("/d", FASTIX_O_WRONLY, 0) == -FASTIX_EISDIR;
    ok &= v_open("/d/f/x", FASTIX_O_RDONLY, 0) == -FASTIX_ENOTDIR &&
          v_open("/d/f/", FASTIX_O_RDONLY, 0) == -FASTIX_ENOTDIR;
    ok &= v_rmdir("/d/.") == -FASTIX_EINVAL && v_rmdir("/d/..") == -FASTIX_ENOTEMPTY &&
          v_rmdir("/") == -FASTIX_EBUSY;
    ok &= v_rename("/d/a", "/d/c", 0) == 0 && v_stat("/d/a", &st, 0) == -FASTIX_ENOENT &&
          v_stat("/d/c", &st, 0) == 0 && (st.st_mode & FASTIX_S_IFMT) == FASTIX_S_IFDIR;
    ok &= v_rename("/d/c", "/d/c/x", 0) == -FASTIX_EINVAL;
    ok &= v_rename("/d/f", "/d/b", 0) == -FASTIX_EISDIR && v_rename("/d/b", "/d/f", 0) == -FASTIX_ENOTDIR;
    ok &= v_put_file("/d/g", "1", 0644) == 0 && v_rename("/d/g", "/d/f", 0) == 0;
    int64_t fd = v_open("/d/f", FASTIX_O_RDONLY, 0);
    ok &= v_read(fd, buf, 2) == 2 && buf[0] == '1' && v_close(fd) == 0;
    ok &= v_stat("/d/g", &st, 0) == -FASTIX_ENOENT;
    ok &= v_rename("/d/f", "/d/b", FASTIX_RENAME_NOREPLACE) == -FASTIX_EEXIST;
    ok &= v_rename("/d/f", "/d/f", 0) == 0 && v_stat("/d/f", &st, 0) == 0;
    ok &= v_mkdir("/d/e", 0755) == 0 && v_rename("/d/c", "/d/e", 0) == 0; /* over empty dir */
    ok &= v_stat("/d", &st, 0) == 0 && st.st_nlink == 4; /* ".", "..", b, e */
    ok &= v_unlink("/d/f") == 0 && v_rmdir("/d/b") == 0 && v_rmdir("/d/e") == 0 &&
          v_rmdir("/d") == 0;
    ok &= v_same_counts(before, v_counts());
    vmtest_report("vfs_dirs_getdents_rename_rmdir", ok);
}

static void two_digits(char *p, int n)
{
    p[0] = (char)('0' + n / 10);
    p[1] = (char)('0' + n % 10);
}

static void t_links(void)
{
    struct vfs_counts before = v_counts();
    struct fastix_stat st;
    char buf[64];
    int ok = v_mkdir("/s", 0755) == 0 && v_put_file("/s/file", "abc", 0644) == 0;
    ok &= v_symlink("file", "/s/l1") == 0 && v_readlink("/s/l1", buf, sizeof(buf)) == 4 &&
          memcmp(buf, "file", 4) == 0;
    ok &= v_stat("/s/l1", &st, 0) == 0 && st.st_size == 4 && (st.st_mode & FASTIX_S_IFMT) == FASTIX_S_IFREG;
    ok &= v_stat("/s/l1", &st, FASTIX_AT_SYMLINK_NOFOLLOW) == 0 &&
          (st.st_mode & FASTIX_S_IFMT) == FASTIX_S_IFLNK;
    ok &= v_readlink("/s/file", buf, 8) == -FASTIX_EINVAL;
    ok &= v_open("/s/l1", FASTIX_O_RDONLY | FASTIX_O_NOFOLLOW, 0) == -FASTIX_ELOOP;
    ok &= v_symlink("loop2", "/s/loop1") == 0 && v_symlink("loop1", "/s/loop2") == 0;
    ok &= v_open("/s/loop1", FASTIX_O_RDONLY, 0) == -FASTIX_ELOOP &&
          v_stat("/s/loop1/x", &st, 0) == -FASTIX_ELOOP;
    /* A dangling symlink with O_CREAT creates its target; O_EXCL does not follow it. */
    ok &= v_symlink("newt", "/s/dang") == 0 &&
          v_open("/s/dang", FASTIX_O_CREAT | FASTIX_O_EXCL | FASTIX_O_WRONLY, 0644) == -FASTIX_EEXIST;
    int64_t fd = v_open("/s/dang", FASTIX_O_CREAT | FASTIX_O_WRONLY, 0644);
    ok &= fd >= 0 && v_close(fd) == 0 && v_stat("/s/newt", &st, 0) == 0;
    ok &= v_symlink("/s", "/s/dl") == 0 && v_stat("/s/dl/file", &st, 0) == 0 &&
          v_stat("/s/dl/", &st, 0) == 0 && (st.st_mode & FASTIX_S_IFMT) == FASTIX_S_IFDIR;
    /* Chain c00 -> c01 -> ... -> c40 -> file: 40 symlinks are fine, 41 are too many. */
    char name[] = "/s/c00", target[] = "c00";
    for (int i = 40; i >= 0; i--) {
        two_digits(name + 4, i);
        two_digits(target + 1, i + 1);
        ok &= v_symlink(i == 40 ? "file" : target, name) == 0;
    }
    ok &= v_stat("/s/c01", &st, 0) == 0 && v_stat("/s/c00", &st, 0) == -FASTIX_ELOOP;
    for (int i = 0; i <= 40; i++) {
        two_digits(name + 4, i);
        ok &= v_unlink(name) == 0;
    }
    /* Hard links; an unlinked file stays usable while open and is freed on the last close. */
    ok &= v_link("/s/file", "/s/hard") == 0 && v_stat("/s/hard", &st, 0) == 0 && st.st_nlink == 2;
    ok &= v_link("/s", "/s/dirlink") == -FASTIX_EPERM && v_link("/s/file", "/s/hard") == -FASTIX_EEXIST;
    ok &= v_unlink("/s/file") == 0 && v_stat("/s/hard", &st, 0) == 0 && st.st_nlink == 1;
    fd = v_open("/s/hard", FASTIX_O_RDWR, 0);
    uint64_t nodes_open = v_counts().nodes;
    ok &= v_unlink("/s/hard") == 0 && v_stat("/s/hard", &st, 0) == -FASTIX_ENOENT;
    ok &= v_fstat(fd, &st) == 0 && st.st_nlink == 0 && v_pwrite(fd, "xyz", 3, 4) == 3 &&
          read_is(fd, 0, "abc\0xyz", 7);
    ok &= v_counts().nodes == nodes_open && v_close(fd) == 0 && v_counts().nodes == nodes_open - 1;
    const char *rm[] = { "/s/l1", "/s/loop1", "/s/loop2", "/s/dang", "/s/newt", "/s/dl" };
    for (unsigned i = 0; i < sizeof(rm) / sizeof(rm[0]); i++) {
        ok &= v_unlink(rm[i]) == 0;
    }
    ok &= v_rmdir("/s") == 0 && v_same_counts(before, v_counts());
    vmtest_report("vfs_symlinks_hardlinks_unlinked_open", ok);
}

void run_vfs_tests(void)
{
    t_file_io();
    t_dirs();
    t_links();
    run_vfs_tests2();
}
