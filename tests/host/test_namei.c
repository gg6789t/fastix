/*
 * test_namei.c - differential test of vfsd's path walk (namei.c) and tmpfs against the
 * host's real file system: each seed builds the same random tree of directories, files and
 * symlinks in a host temp directory and in tmpfs, then resolves random paths (".", "..",
 * symlinks, trailing slashes) on both sides; error codes and results must match. Each tree sits
 * under DEPTH nested directories so ".." never leaves the identical area on either side.
 * Unmounting each seed's tmpfs must succeed, proving no vnode reference leaked.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fastix/errno.h>

#include "../../servers/vfsd/namei.h"
#include "../../servers/vfsd/tmpfs.h"

#define SEEDS 200
#define STEPS 400
#define DEPTH 100 /* more than the ".." a path can produce: 5 + 40 symlinks x 1 */
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            fprintf(stderr, "seed %u step %u: check failed: %s (line %d)\n", seed,  \
                    step, #c, __LINE__);                                             \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

static unsigned seed, step;
static uint64_t rng;
static const struct cred root_cred = { 0, 0 };
static struct vnode *top;    /* bottom of the DEPTH stack: relative walks start here */
static struct vnode *fstop;  /* the seed's tmpfs root: ".." stops here */
static unsigned tag;

/* ---- hosted shims for vfsd ---- */

static void *pages[1 << 16];
static size_t n_pages;

static int host_page(void *ctx, void **out)
{
    (void)ctx;
    void *p = aligned_alloc(FASTIX_PAGE_SIZE, FASTIX_PAGE_SIZE);
    if (p == NULL || n_pages == sizeof(pages) / sizeof(pages[0])) {
        free(p);
        return -FASTIX_ENOMEM;
    }
    memset(p, 0, FASTIX_PAGE_SIZE);
    pages[n_pages++] = p;
    *out = p;
    return 0;
}

struct slab_provider vfs_page_provider(void)
{
    return (struct slab_provider){ host_page, NULL };
}

void vfs_now(struct fastix_timespec *ts)
{
    ts->tv_sec = 0;
    ts->tv_nsec = 0;
}

/* ---- random paths ---- */

static unsigned rnd(unsigned n)
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned)(rng >> 33) % n;
}

static const char *const names[] = { "a", "b", "c", "l1", "l2" };

static const char *any_name(void)
{
    return names[rnd(5)];
}

/* Random relative path of 1..5 components; "." and ".." only if dots, '/' at the end if slash. */
static void rand_path(char *out, int dots, int slash)
{
    unsigned n = 1 + rnd(5);
    out[0] = '\0';
    for (unsigned i = 0; i < n; i++) {
        unsigned r = rnd(8);
        const char *c = (dots && r == 6) ? "." : (dots && r == 7) ? ".." : any_name();
        strcat(out, c);
        if (i + 1 < n) {
            strcat(out, "/");
        }
    }
    if (slash && rnd(4) == 0) {
        strcat(out, "/");
    }
}

/* Host errno -> Fastix errno for the codes a path walk can produce. */
static int64_t host_err(int e)
{
    switch (e) {
    case ENOENT: return -FASTIX_ENOENT;
    case ENOTDIR: return -FASTIX_ENOTDIR;
    case ELOOP: return -FASTIX_ELOOP;
    case ENAMETOOLONG: return -FASTIX_ENAMETOOLONG;
    case EACCES: return -FASTIX_EACCES;
    default: return -10000 - e;
    }
}

/* ---- comparing one path ---- */

static void dir_names(struct vnode *d, char *out)
{
    struct dir_entry e;
    uint64_t cookie = 0;
    char list[5] = { 0 };
    while (d->ops->readdir(d, cookie, &e) == 1) {
        for (unsigned i = 0; i < 5; i++) {
            if (e.len == strlen(names[i]) && memcmp(e.name, names[i], e.len) == 0) {
                list[i] = 1;
            }
        }
        cookie = e.cookie;
    }
    for (unsigned i = 0; i < 5; i++) {
        out[i] = list[i] ? 'y' : 'n';
    }
    out[5] = '\0';
}

static void host_dir_names(const char *path, char *out)
{
    for (unsigned i = 0; i < 5; i++) {
        char p[512];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", path, names[i]);
        out[i] = lstat(p, &st) == 0 ? 'y' : 'n';
    }
    out[5] = '\0';
}

/* Resolves `path` on both sides (follow = stat, else lstat) and compares the outcome. */
static void compare(const char *path, int follow)
{
    struct stat st;
    /* Linux (our ABI) follows a final symlink when the path ends with '/', and then needs a
     * directory; macOS does neither, so the host side applies those two rules itself. */
    size_t plen = strlen(path);
    int slash = plen > 0 && path[plen - 1] == '/';
    int hr = (follow || slash) ? stat(path, &st) : lstat(path, &st);
    int64_t hres = hr == 0 ? 0 : host_err(errno);
    if (hr == 0 && slash && !S_ISDIR(st.st_mode)) {
        hres = -FASTIX_ENOTDIR;
    }
    static struct walk w;
    int64_t vres = vfs_walk(&root_cred, fstop, top, path, follow ? WALK_FOLLOW : 0, &w);
    if (vres != hres) {
        fprintf(stderr, "seed %u step %u: %s(\"%s\") host %lld vfsd %lld\n", seed, step,
                follow ? "stat" : "lstat", path, (long long)hres, (long long)vres);
        exit(1);
    }
    if (vres != 0) {
        return;
    }
    struct vnode *v = w.v;
    CHECK((v->mode & FASTIX_S_IFMT) == (st.st_mode & S_IFMT));
    if (S_ISREG(st.st_mode)) {
        char a[32] = { 0 }, b[32] = { 0 };
        int fd = open(path, O_RDONLY);
        CHECK(fd >= 0 && read(fd, a, sizeof(a) - 1) >= 0 && close(fd) == 0);
        CHECK(v->ops->read(v, 0, b, sizeof(b) - 1) >= 0 && strcmp(a, b) == 0);
    } else if (S_ISLNK(st.st_mode)) {
        char a[64] = { 0 }, b[64] = { 0 };
        size_t n = 0;
        CHECK(readlink(path, a, sizeof(a) - 1) >= 0 && v->ops->readlink(v, b, sizeof(b) - 1, &n) == 0);
        CHECK(strcmp(a, b) == 0);
    } else {
        char a[6], b[6];
        host_dir_names(path, a);
        dir_names(v, b);
        if (strcmp(a, b) != 0) {
            fprintf(stderr, "seed %u step %u: \"%s\" lists host %s vfsd %s\n", seed, step, path, a, b);
            exit(1);
        }
    }
    walk_done(&w);
}

/* ---- applying the same change to both trees ---- */

/* Parent directory vnode of `dir` (a path or ""), or NULL. */
static struct vnode *vdir(const char *dir, struct walk *w)
{
    if (dir[0] == '\0') {
        vnode_get(top);
        return top;
    }
    if (vfs_walk(&root_cred, fstop, top, dir, WALK_FOLLOW, w) != 0) {
        return NULL;
    }
    struct vnode *v = w->v;
    w->v = NULL;
    return v;
}

static void join(char *out, const char *dir, const char *name)
{
    if (dir[0] == '\0') {
        strcpy(out, name);
    } else {
        snprintf(out, 256, "%s/%s", dir, name);
    }
}

/* One random change. It is applied to tmpfs only if the host accepted it, so both trees stay
 * equal; then the tmpfs side must succeed too (which also checks the walk). */
static void mutate(void)
{
    static struct walk w, w2;
    char dir[256], dir2[256], path[256], path2[256], target[64];
    unsigned op = rnd(6);
    rand_path(dir, 1, 0);
    if (rnd(3) == 0) {
        dir[0] = '\0';
    }
    const char *name = any_name();
    join(path, dir, name);
    struct stat st;
    int is_dir = lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
    int ok;
    struct vnode *d, *out = NULL;
    switch (op) {
    case 0: /* mkdir */
        if (mkdir(path, 0755) != 0) return;
        d = vdir(dir, &w);
        CHECK(d && d->ops->create(d, name, strlen(name), FASTIX_S_IFDIR | 0755, &root_cred, NULL, 0, &out) == 0);
        break;
    case 1: { /* new file with a unique content tag */
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd < 0) return;
        char t[16];
        int n = snprintf(t, sizeof(t), "f%u", ++tag);
        CHECK(write(fd, t, (size_t)n) == n && close(fd) == 0);
        d = vdir(dir, &w);
        CHECK(d && d->ops->create(d, name, strlen(name), FASTIX_S_IFREG | 0644, &root_cred, NULL, 0, &out) == 0);
        CHECK(out->ops->write(out, 0, t, (size_t)n) == n);
        break;
    }
    case 2: /* symlink to a path with at most one ".." */
        rand_path(target, 0, 0);
        if (rnd(3) == 0) {
            snprintf(target, sizeof(target), "../%s", any_name());
        }
        if (symlink(target, path) != 0) return;
        d = vdir(dir, &w);
        CHECK(d && d->ops->create(d, name, strlen(name), FASTIX_S_IFLNK | 0777, &root_cred, target, strlen(target), &out) == 0);
        break;
    case 3: /* unlink a non-directory, or rmdir an empty directory */
        ok = is_dir ? rmdir(path) == 0 : unlink(path) == 0;
        if (!ok) return;
        d = vdir(dir, &w);
        CHECK(d && d->ops->unlink(d, name, strlen(name)) == 0);
        break;
    case 4: { /* rename */
        rand_path(dir2, 1, 0);
        if (rnd(2) == 0) dir2[0] = '\0';
        const char *name2 = any_name();
        join(path2, dir2, name2);
        struct stat st2;
        int same = lstat(path, &st) == 0 && lstat(path2, &st2) == 0 && st.st_ino == st2.st_ino;
        if (rename(path, path2) != 0 || same) return; /* links of one file: POSIX no-op */
        d = vdir(dir, &w);
        struct vnode *d2 = vdir(dir2, &w2);
        CHECK(d && d2 && d->ops->rename(d, name, strlen(name), d2, name2, strlen(name2)) == 0);
        vnode_put(d2);
        break;
    }
    default: { /* hard link to a regular file */
        rand_path(dir2, 1, 0);
        join(path2, dir2, any_name());
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || link(path, path2) != 0) return;
        const char *slash = strrchr(path2, '/');
        char d2path[256] = "";
        const char *name2 = path2;
        if (slash != NULL) {
            memcpy(d2path, path2, (size_t)(slash - path2));
            d2path[slash - path2] = '\0';
            name2 = slash + 1;
        }
        CHECK(vfs_walk(&root_cred, fstop, top, path, 0, &w) == 0);
        struct vnode *f = w.v;
        w.v = NULL;
        walk_done(&w);
        d = vdir(d2path, &w2);
        CHECK(d && d->ops->link(d, name2, strlen(name2), f) == 0);
        vnode_put(f);
        break;
    }
    }
    if (out != NULL) {
        vnode_put(out);
    }
    vnode_put(d);
    walk_done(&w);
    walk_done(&w2);
}

/* ---- one seed ---- */

static void run_seed(struct vnode *mountpoint, const char *hostdir)
{
    rng = seed * 0x9E3779B97F4A7C15ULL + 1;
    struct vnode *fsroot, *out;
    CHECK(tmpfs_new(1 << 20, 0755, &root_cred, &fsroot) == 0 && vfs_mount(mountpoint, fsroot) == 0);
    fstop = fsroot;
    CHECK(chdir(hostdir) == 0);
    /* DEPTH nested "p" directories on both sides; the tree lives at the bottom. */
    struct vnode *cur = fsroot;
    vnode_get(cur);
    for (int i = 0; i < DEPTH; i++) {
        CHECK(mkdir("p", 0755) == 0 && chdir("p") == 0);
        CHECK(cur->ops->create(cur, "p", 1, FASTIX_S_IFDIR | 0755, &root_cred, NULL, 0, &out) == 0);
        vnode_put(cur);
        cur = out;
    }
    top = cur;
    for (step = 0; step < STEPS; step++) {
        char path[256];
        if (rnd(3) == 0) {
            mutate();
        } else {
            rand_path(path, 1, 1);
            compare(path, (int)rnd(2));
        }
    }
    vnode_put(top);
    vnode_get(fsroot);
    CHECK(vfs_umount(fsroot) == 0); /* fails with EBUSY if any walk leaked a reference */
    struct tmpfs_stats ts = tmpfs_stats();
    CHECK(ts.pages == 0);
}

int main(void)
{
    char tmpl[] = "/tmp/fastix_namei_XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    tmpfs_module_init();
    struct vnode *base, *mp;
    struct mount *m;
    CHECK(tmpfs_new(1 << 20, 0755, &root_cred, &base) == 0 && vfs_mount_root(base, &m) == 0);
    CHECK(base->ops->create(base, "x", 1, FASTIX_S_IFDIR | 0755, &root_cred, NULL, 0, &mp) == 0);
    uint64_t base_nodes = tmpfs_stats().nodes;
    for (seed = 1; seed <= SEEDS; seed++) {
        char dir[128], cmd[160];
        snprintf(dir, sizeof(dir), "%s/s%u", tmpl, seed);
        CHECK(mkdir(dir, 0755) == 0);
        run_seed(mp, dir);
        CHECK(tmpfs_stats().nodes == base_nodes);
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        CHECK(chdir(tmpl) == 0);
        CHECK(system(cmd) == 0);
    }
    vnode_put(mp);
    rmdir(tmpl);
    for (size_t i = 0; i < n_pages; i++) {
        free(pages[i]);
    }
    printf("test_namei: %d seeds x %d steps match the host file system\n", SEEDS, STEPS);
    return 0;
}
