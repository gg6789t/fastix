/*
 * test_vfs2.c - QEMU tests for vfsd (Phase 4C) part 2: FD sharing (dup, fork), close-on-exec
 * through execve and posix_spawn, pipes in vfsd's FD table, cwd and mounts, permissions,
 * and error cases. Every test also checks that vfsd and procd leak nothing.
 */
#include <stdint.h>

#include <fastix/abi.h>
#include <fastix/errno.h>
#include <fastix/fs_abi.h>
#include <fastix/signal_abi.h>
#include <fastix_sys/sys.h>
#include <libos/mem.h>

#include "vfsutil.h"
#include "vmtest.h"

#define NEW_RW (FASTIX_O_CREAT | FASTIX_O_RDWR)

static void u2s(char *out, uint64_t v)
{
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    for (int i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = '\0';
}

/* Runs exec_helper "check_fds open closed" by fork+execve (spawn = 0) or posix_spawn. */
static int exec_sees(int64_t open_fd, int64_t closed_fd, int spawn)
{
    char a2[24], a3[24];
    u2s(a2, (uint64_t)open_fd);
    u2s(a3, (uint64_t)closed_fd);
    const char *argv[] = { "/bin/exec_helper", "check_fds", a2, a3, NULL };
    const char *envp[] = { NULL };
    int64_t pid;
    if (spawn) {
        struct fastix_spawn_args sa = { .path = (uint64_t)(uintptr_t)argv[0],
                                        .argv = (uint64_t)(uintptr_t)argv,
                                        .envp = (uint64_t)(uintptr_t)envp };
        pid = fastix_sys1(FASTIX_SYS_posix_spawn_fast, (uint64_t)(uintptr_t)&sa);
    } else {
        pid = fastix_sys0(FASTIX_SYS_fork);
        if (pid == 0) {
            (void)fastix_sys3(FASTIX_SYS_execve, (uint64_t)(uintptr_t)argv[0],
                              (uint64_t)(uintptr_t)argv, (uint64_t)(uintptr_t)envp);
            vmtest_exit(1);
        }
    }
    int st = 0;
    return pid > 0 && vmtest_wait4(pid, &st, 0) == pid && st == FASTIX_W_EXITED(92);
}

static void t_fd_sharing(void)
{
    struct vm_test_stats pbefore = vmtest_get_stats();
    struct vfs_counts before = v_counts();
    int64_t fd = v_open("/fdt", NEW_RW | FASTIX_O_CLOEXEC, 0600);
    int64_t d = v_dup(fd);
    int ok = fd >= 0 && d >= 0 && v_fcntl(fd, FASTIX_F_GETFD, 0) == FASTIX_FD_CLOEXEC &&
             v_fcntl(d, FASTIX_F_GETFD, 0) == 0;
    /* dup'ed FDs share the offset and the status flags, but not FD_CLOEXEC. */
    ok &= v_write(fd, "12345", 5) == 5 && v_lseek(d, 0, FASTIX_SEEK_CUR) == 5;
    ok &= v_fcntl(d, FASTIX_F_SETFL, FASTIX_O_APPEND) == 0 &&
          v_fcntl(fd, FASTIX_F_GETFL, 0) == (FASTIX_O_RDWR | FASTIX_O_APPEND);
    ok &= v_dup3(fd, 100, FASTIX_O_CLOEXEC) == 100 && v_fcntl(100, FASTIX_F_GETFD, 0) == FASTIX_FD_CLOEXEC;
    ok &= v_dup3(fd, fd, 0) == -FASTIX_EINVAL && v_dup3(fd, 5000, 0) == -FASTIX_EBADF;
    ok &= v_fcntl(fd, FASTIX_F_DUPFD, 200) == 200 && v_fcntl(fd, FASTIX_F_DUPFD, 5000) == -FASTIX_EINVAL;
    /* A forked child shares the open file description, so its seek moves ours. */
    int64_t pid = fastix_sys0(FASTIX_SYS_fork);
    if (pid == 0) {
        vmtest_exit(v_lseek(d, 1, FASTIX_SEEK_SET) == 1 ? 0 : 1);
    }
    int st = -1;
    ok &= vmtest_wait4(pid, &st, 0) == pid && st == 0 && v_lseek(fd, 0, FASTIX_SEEK_CUR) == 1;
    /* execve and posix_spawn close FD_CLOEXEC descriptors (fd, 100) and keep the others (d). */
    ok &= exec_sees(d, fd, 0) && exec_sees(d, 100, 1) && exec_sees(200, fd, 0);
    ok &= v_close(100) == 0 && v_close(200) == 0 && v_close(d) == 0 && v_close(fd) == 0;
    ok &= v_unlink("/fdt") == 0;
    ok &= v_same_counts(before, v_counts()) && vmtest_same_stats(pbefore, vmtest_get_stats());
    vmtest_report("vfs_dup_fcntl_fork_shared_offset_cloexec", ok);
}

static void t_pipes_in_vfsd(void)
{
    struct vm_test_stats pbefore = vmtest_get_stats();
    struct vfs_counts before = v_counts();
    struct fastix_stat st;
    int32_t p[2] = { -1, -1 };
    char buf[8];
    int ok = fastix_sys2(FASTIX_SYS_pipe2, (uint64_t)(uintptr_t)p, 0) == 0 && p[0] >= 0 && p[1] >= 0;
    ok &= v_fstat(p[0], &st) == 0 && (st.st_mode & FASTIX_S_IFMT) == FASTIX_S_IFIFO;
    ok &= v_lseek(p[0], 0, FASTIX_SEEK_SET) == -FASTIX_ESPIPE &&
          v_pread(p[0], buf, 1, 0) == -FASTIX_ESPIPE;
    /* The read end goes non-blocking through vfsd's F_SETFL; procd must see it. */
    ok &= v_fcntl(p[0], FASTIX_F_SETFL, FASTIX_O_NONBLOCK) == 0 &&
          v_read(p[0], buf, 1) == -FASTIX_EAGAIN;
    int64_t w2 = v_dup(p[1]);
    ok &= v_write(w2, "hi", 2) == 2 && v_close(w2) == 0 && v_read(p[0], buf, 8) == 2;
    ok &= v_read(p[1], buf, 1) == -FASTIX_EBADF && v_write(p[0], "x", 1) == -FASTIX_EBADF;
    ok &= v_close(p[1]) == 0 && v_read(p[0], buf, 1) == 0; /* last writer gone: EOF */
    ok &= v_close(p[0]) == 0 && v_read(p[0], buf, 1) == -FASTIX_EBADF;
    /* eventfd2 FDs also live in vfsd's table. */
    int64_t efd = fastix_sys2(FASTIX_SYS_eventfd2, 5, 0);
    uint64_t val = 0;
    ok &= efd >= 0 && v_read(efd, &val, 8) == 8 && val == 5 && v_close(efd) == 0;
    ok &= v_same_counts(before, v_counts()) && vmtest_same_stats(pbefore, vmtest_get_stats());
    vmtest_report("vfs_pipes_eventfd_in_vfsd_fd_table", ok);
}

static void t_cwd_mount(void)
{
    struct vfs_counts before = v_counts();
    struct fastix_stat st, st2;
    char cwd[64];
    int ok = v_mkdir("/m", 0755) == 0 && v_mkdir("/m/sub", 0755) == 0 && v_mkdir("/m/mnt", 0755) == 0;
    ok &= v_chdir("/m/sub") == 0 && v_getcwd(cwd, sizeof(cwd)) == 7 && str_same(cwd, "/m/sub");
    int64_t fd = v_open("x", NEW_RW, 0644); /* relative to the cwd */
    ok &= fd >= 0 && v_close(fd) == 0 && v_stat("/m/sub/x", &st, 0) == 0;
    ok &= v_chdir("..") == 0 && v_getcwd(cwd, sizeof(cwd)) == 3 && str_same(cwd, "/m");
    ok &= v_chdir("/../..") == 0 && v_getcwd(cwd, sizeof(cwd)) == 2 && str_same(cwd, "/");
    ok &= v_getcwd(cwd, 1) == -FASTIX_ERANGE && v_stat("/../../m/./sub/../sub/x", &st, 0) == 0;
    /* A tmpfs mounted on /m/mnt: new device, ".." leads back across the mount. */
    ok &= v_mount_tmpfs("/m/mnt") == 0 && v_stat("/m/mnt", &st, 0) == 0 && v_stat("/m", &st2, 0) == 0;
    ok &= st.st_dev != st2.st_dev && v_put_file("/m/mnt/y", "y", 0644) == 0;
    ok &= v_chdir("/m/mnt") == 0 && v_getcwd(cwd, sizeof(cwd)) == 7 && str_same(cwd, "/m/mnt");
    ok &= v_stat("..", &st, 0) == 0 && st.st_ino == st2.st_ino && st.st_dev == st2.st_dev;
    ok &= v_link("/m/mnt/y", "/m/ylink") == -FASTIX_EXDEV &&
          v_rename("/m/mnt/y", "/m/y", 0) == -FASTIX_EXDEV;
    ok &= v_umount("/m/mnt") == -FASTIX_EBUSY && v_rmdir("/m/mnt") == -FASTIX_EBUSY; /* in use */
    ok &= v_chdir("/") == 0 && v_umount("/m/mnt") == 0 && v_stat("/m/mnt/y", &st, 0) == -FASTIX_ENOENT;
    ok &= v_umount("/m/mnt") == -FASTIX_EINVAL;
    /* fchdir, and a cwd that is removed while in use. */
    fd = v_open("/m", FASTIX_O_RDONLY | FASTIX_O_DIRECTORY, 0);
    ok &= v_fchdir(fd) == 0 && v_getcwd(cwd, sizeof(cwd)) == 3 && v_close(fd) == 0;
    ok &= v_mkdir("/m/gone", 0755) == 0 && v_chdir("/m/gone") == 0 && v_rmdir("/m/gone") == 0;
    ok &= v_getcwd(cwd, sizeof(cwd)) == -FASTIX_ENOENT && v_chdir("/") == 0;
    ok &= v_unlink("/m/sub/x") == 0 && v_rmdir("/m/sub") == 0 && v_rmdir("/m/mnt") == 0 &&
          v_rmdir("/m") == 0;
    ok &= v_same_counts(before, v_counts());
    vmtest_report("vfs_cwd_getcwd_mount_dotdot_umount", ok);
}

/* As uid 1000 / gid 100 (in a child): returns 0 or the number of the first failed check. */
static int unprivileged_checks(void)
{
    struct fastix_stat st;
    int64_t fd;
    if (v_open("/p/secret", FASTIX_O_RDONLY, 0) != -FASTIX_EACCES) return 1;
    if (v_stat("/p/secret", &st, 0) != 0) return 2;
    if (v_open("/p/closed/inner", FASTIX_O_RDONLY, 0) != -FASTIX_EACCES) return 3;
    if (v_open("/p/new", NEW_RW, 0644) != -FASTIX_EACCES) return 4;
    if (v_access("/p/secret", FASTIX_R_OK) != -FASTIX_EACCES) return 5;
    if (v_access("/p/secret", FASTIX_F_OK) != 0) return 6;
    if (v_chmod("/p/secret", 0644) != -FASTIX_EPERM) return 7;
    if (v_mount_tmpfs("/p") != -FASTIX_EPERM) return 8;
    if (v_unlink("/tx/rootfile") != -FASTIX_EPERM) return 9; /* sticky directory */
    if ((fd = v_open("/tx/mine", NEW_RW, 0666)) < 0 || v_fstat(fd, &st) != 0) return 10;
    if (st.st_uid != 1000 || st.st_gid != 100 || (st.st_mode & 0777u) != (0666u & ~022u)) return 11;
    if (v_close(fd) != 0 || v_unlink("/tx/mine") != 0) return 12;
    return 0;
}

static void t_permissions(void)
{
    struct vfs_counts before = v_counts();
    int ok = v_mkdir("/p", 0755) == 0 && v_put_file("/p/secret", "s", 0600) == 0;
    ok &= v_mkdir("/p/closed", 0700) == 0 && v_put_file("/p/closed/inner", "i", 0644) == 0;
    ok &= v_mkdir("/tx", 0777) == 0 && v_chmod("/tx", 01777) == 0 &&
          v_put_file("/tx/rootfile", "r", 0644) == 0;
    int64_t pid = fastix_sys0(FASTIX_SYS_fork);
    if (pid == 0) {
        int bad = fastix_sys3(FASTIX_SYS_setresgid, 100, 100, 100) != 0 ||
                  fastix_sys3(FASTIX_SYS_setresuid, 1000, 1000, 1000) != 0;
        vmtest_exit(bad ? 99 : unprivileged_checks());
    }
    int st = -1;
    ok &= vmtest_wait4(pid, &st, 0) == pid && st == 0;
    int64_t rfd = v_open("/p/closed/inner", FASTIX_O_RDONLY, 0); /* root may */
    ok &= rfd >= 0 && v_close(rfd) == 0;
    ok &= v_unlink("/p/closed/inner") == 0 && v_rmdir("/p/closed") == 0 && v_unlink("/p/secret") == 0;
    ok &= v_rmdir("/p") == 0 && v_unlink("/tx/rootfile") == 0 && v_rmdir("/tx") == 0;
    ok &= v_same_counts(before, v_counts());
    vmtest_report("vfs_permissions_sticky_access_umask", ok);
}

/* Opens "/" until the FD table is full; returns how many opens succeeded, then closes all. */
static int64_t fill_fd_table(void)
{
    int64_t n = 0;
    while (v_open("/", FASTIX_O_RDONLY, 0) >= 0) {
        n++;
    }
    if (v_open("/", FASTIX_O_RDONLY, 0) != -FASTIX_EMFILE) {
        n = -1;
    }
    for (int64_t i = 0; i < FASTIX_DEFAULT_NOFILE; i++) {
        (void)v_close(i);
    }
    return n;
}

static void t_errors_and_limits(void)
{
    (void)fill_fd_table(); /* warm-up: vfsd's OFD cache reaches its steady size */
    struct vm_test_stats pbefore = vmtest_get_stats();
    struct vfs_counts before = v_counts();
    char outside[8] = "/";
    int ok = fastix_vfs4(FASTIX_SYS_openat, (uint64_t)(int64_t)FASTIX_AT_FDCWD,
                         (uint64_t)(uintptr_t)outside, 0, 0) == -FASTIX_EFAULT;
    ok &= v_open("", FASTIX_O_RDONLY, 0) == -FASTIX_ENOENT;
    char longname[300];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[0] = '/';
    longname[sizeof(longname) - 1] = '\0';
    ok &= v_open(longname, FASTIX_O_RDONLY, 0) == -FASTIX_ENAMETOOLONG;
    ok &= v_close(-1) == -FASTIX_EBADF && v_close(5000) == -FASTIX_EBADF &&
          v_fcntl(999, FASTIX_F_GETFD, 0) == -FASTIX_EBADF;
    int64_t fd = v_open("/", FASTIX_O_RDONLY, 0);
    ok &= fastix_vfs3(FASTIX_SYS_getdents64, (uint64_t)fd, (uint64_t)(uintptr_t)outside, 8) ==
          -FASTIX_EFAULT;
    ok &= v_close(fd) == 0;
    ok &= fill_fd_table() == FASTIX_DEFAULT_NOFILE; /* then EMFILE; closing frees every slot */
    ok &= v_same_counts(before, v_counts()) && vmtest_same_stats(pbefore, vmtest_get_stats());
    vmtest_report("vfs_errors_efault_enametoolong_emfile", ok);
}

void run_vfs_tests2(void)
{
    t_fd_sharing();
    t_pipes_in_vfsd();
    t_cwd_mount();
    t_permissions();
    t_errors_and_limits();
}
