/* client.c - vfsd clients, FD tables and OFDs (see client.h, AGENTS.md 6.4). */
#include <fastix/cspace.h>
#include <fastix/errno.h>
#include <fastix/mman.h>
#include <fastix/vdso.h>
#include <fastix_sys/sys.h>
#include <libos/mem.h>

#include "client.h"
#include "vfsd.h"

#define NOFILE    FASTIX_DEFAULT_NOFILE
#define FD_FLAGS  (FASTIX_O_ACCMODE | FASTIX_O_APPEND | FASTIX_O_NONBLOCK)

static struct client *clients; /* FASTIX_VFS_MAX_CLIENTS, demand-paged */
static struct slab_cache ofd_cache;
static struct vnode *root_vnode;
static uint64_t n_clients;

int clients_init(struct vnode *root)
{
    slab_init(&ofd_cache, "ofd", sizeof(struct ofd), vfs_page_provider());
    uint64_t bytes = (sizeof(struct client) * FASTIX_VFS_MAX_CLIENTS + FASTIX_PAGE_SIZE - 1) &
                     ~(uint64_t)(FASTIX_PAGE_SIZE - 1);
    int64_t addr = fastix_sys6(FASTIX_SYS_mmap, 0, bytes, FASTIX_PROT_READ | FASTIX_PROT_WRITE,
                               FASTIX_MAP_PRIVATE | FASTIX_MAP_ANONYMOUS, (uint64_t)-1, 0);
    if (addr < 0) {
        return (int)addr;
    }
    clients = (struct client *)(uintptr_t)addr;
    root_vnode = root;
    return 0;
}

struct vnode *vfs_root(void)
{
    return root_vnode;
}

uint64_t client_count(void)
{
    return n_clients;
}

uint64_t ofd_count(void)
{
    return ofd_cache.in_use;
}

struct client *client_from_badge(uint64_t badge)
{
    uint32_t slot = fastix_badge_tid_slot(badge);
    if (fastix_badge_type(badge) != FASTIX_BADGE_SYSCALL || slot >= FASTIX_VFS_MAX_CLIENTS) {
        return NULL;
    }
    struct client *c = &clients[slot];
    return (c->used && c->pid == fastix_badge_pid(badge)) ? c : NULL;
}

static const struct fastix_procinfo *info_of(const struct client *c)
{
    return (const struct fastix_procinfo *)(uintptr_t)client_view(c->slot, FASTIX_VFSD_INFO_OFF);
}

struct ids {
    uint32_t uid, euid, gid, egid, umask;
};

/* Reads the client's procinfo under its seqlock (procd updates it on setuid/umask). */
static struct ids read_ids(const struct client *c)
{
    const struct fastix_procinfo *pi = info_of(c);
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&pi->seq, memory_order_acquire);
        struct ids r = { pi->uid, pi->euid, pi->gid, pi->egid, pi->umask };
        atomic_thread_fence(memory_order_acquire);
        if ((s1 & 1u) == 0 && atomic_load_explicit(&pi->seq, memory_order_relaxed) == s1) {
            return r;
        }
    }
}

struct cred client_cred(const struct client *c)
{
    struct ids r = read_ids(c);
    return (struct cred){ r.euid, r.egid };
}

struct cred client_real_cred(const struct client *c)
{
    struct ids r = read_ids(c);
    return (struct cred){ r.uid, r.gid };
}

uint32_t client_umask(const struct client *c)
{
    return read_ids(c).umask & 0777u;
}

/* ---- FD table + export ---- */

static void export_fd(const struct client *c, int64_t fd, uint32_t word)
{
    uint32_t *tab = (uint32_t *)(uintptr_t)client_view(c->slot, FASTIX_VFSD_FDEXP_OFF);
    __atomic_store_n(&tab[fd], word, __ATOMIC_RELEASE);
}

static uint32_t export_word(const struct ofd *o)
{
    return o->owner == FASTIX_OWNER_PROCD ? fastix_fdexp_obj(FASTIX_OWNER_PROCD, o->obj)
                                          : fastix_fdexp_obj(FASTIX_OWNER_VFSD, 0);
}

static bool fd_cloexec(const struct client *c, int64_t fd)
{
    return (c->cloexec[fd / 64] >> (fd % 64)) & 1u;
}

static void fd_set(struct client *c, int64_t fd, struct ofd *o, bool cloexec)
{
    c->fds[fd] = o;
    uint64_t bit = 1ULL << (fd % 64);
    c->cloexec[fd / 64] = cloexec ? (c->cloexec[fd / 64] | bit) : (c->cloexec[fd / 64] & ~bit);
    export_fd(c, fd, export_word(o));
}

/* Removes fd; the export word is cleared before the caller drops the OFD reference, so procd
 * never finds an object that is about to be freed. */
static struct ofd *fd_clear(struct client *c, int64_t fd)
{
    struct ofd *o = c->fds[fd];
    export_fd(c, fd, 0);
    c->fds[fd] = NULL;
    c->cloexec[fd / 64] &= ~(1ULL << (fd % 64));
    return o;
}

struct ofd *fd_get(const struct client *c, int64_t fd)
{
    return (fd >= 0 && fd < NOFILE) ? c->fds[fd] : NULL;
}

int fd_install(struct client *c, struct ofd *o, int64_t min_fd, bool cloexec)
{
    for (int64_t fd = min_fd < 0 ? 0 : min_fd; fd < NOFILE; fd++) {
        if (c->fds[fd] == NULL) {
            fd_set(c, fd, o, cloexec);
            return (int)fd;
        }
    }
    return -FASTIX_EMFILE;
}

/* ---- OFDs ---- */

struct ofd *ofd_new_vnode(struct vnode *v, uint32_t flags)
{
    struct ofd *o = slab_alloc(&ofd_cache);
    if (o == NULL) {
        return NULL;
    }
    *o = (struct ofd){ .refs = 1, .flags = flags & FD_FLAGS, .owner = FASTIX_OWNER_VFSD, .v = v };
    return o;
}

void ofd_put(struct ofd *o)
{
    if (--o->refs != 0) {
        return;
    }
    if (o->owner == FASTIX_OWNER_PROCD) {
        uint64_t obj = o->obj;
        (void)procd_call(FASTIX_SRV_OBJ_RELEASE, 1, &obj);
    } else {
        vnode_put(o->v);
    }
    slab_free(&ofd_cache, o);
}

/* ---- FD syscalls ---- */

int64_t sys_close(struct client *c, int64_t fd)
{
    if (fd_get(c, fd) == NULL) {
        return -FASTIX_EBADF;
    }
    ofd_put(fd_clear(c, fd));
    return 0;
}

int64_t sys_dup(struct client *c, int64_t fd)
{
    struct ofd *o = fd_get(c, fd);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    o->refs++;
    int nfd = fd_install(c, o, 0, false);
    if (nfd < 0) {
        o->refs--;
    }
    return nfd;
}

int64_t sys_dup3(struct client *c, int64_t oldfd, int64_t newfd, uint64_t flags)
{
    if ((flags & ~(uint64_t)FASTIX_O_CLOEXEC) != 0 || oldfd == newfd) {
        return -FASTIX_EINVAL;
    }
    struct ofd *o = fd_get(c, oldfd);
    if (o == NULL || newfd < 0 || newfd >= NOFILE) {
        return -FASTIX_EBADF;
    }
    o->refs++;
    struct ofd *prev = c->fds[newfd] != NULL ? fd_clear(c, newfd) : NULL;
    fd_set(c, newfd, o, (flags & FASTIX_O_CLOEXEC) != 0);
    if (prev != NULL) {
        ofd_put(prev);
    }
    return newfd;
}

static int64_t setfl(struct ofd *o, uint64_t arg)
{
    uint32_t settable = FASTIX_O_APPEND | FASTIX_O_NONBLOCK;
    o->flags = (o->flags & ~settable) | ((uint32_t)arg & settable);
    if (o->owner == FASTIX_OWNER_PROCD) {
        uint64_t mr[2] = { o->obj, o->flags };
        (void)procd_call(FASTIX_SRV_OBJ_SETFL, 2, mr);
    }
    return 0;
}

int64_t sys_fcntl(struct client *c, int64_t fd, uint64_t cmd, uint64_t arg)
{
    struct ofd *o = fd_get(c, fd);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    switch (cmd) {
    case FASTIX_F_GETFD:
        return fd_cloexec(c, fd) ? FASTIX_FD_CLOEXEC : 0;
    case FASTIX_F_SETFD:
        fd_set(c, fd, o, (arg & FASTIX_FD_CLOEXEC) != 0);
        return 0;
    case FASTIX_F_GETFL:
        return o->flags;
    case FASTIX_F_SETFL:
        return setfl(o, arg);
    case FASTIX_F_DUPFD:
    case FASTIX_F_DUPFD_CLOEXEC: {
        if (arg >= NOFILE) {
            return -FASTIX_EINVAL;
        }
        o->refs++;
        int nfd = fd_install(c, o, (int64_t)arg, cmd == FASTIX_F_DUPFD_CLOEXEC);
        if (nfd < 0) {
            o->refs--;
        }
        return nfd;
    }
    default:
        return -FASTIX_EINVAL;
    }
}

/* ---- procd work items ---- */

static int64_t client_new(struct client *c, uint32_t slot, uint32_t pid, uint64_t parent,
                          bool skip_cloexec)
{
    memset(c, 0, sizeof(*c));
    c->used = true;
    c->pid = pid;
    c->slot = slot;
    const struct client *p = parent < FASTIX_VFS_MAX_CLIENTS ? &clients[parent] : NULL;
    if (p == NULL || !p->used) {
        c->cwd = root_vnode;
        vnode_get(c->cwd);
        n_clients++;
        return 0;
    }
    for (int64_t fd = 0; fd < NOFILE; fd++) {
        if (p->fds[fd] != NULL && !(skip_cloexec && fd_cloexec(p, fd))) {
            p->fds[fd]->refs++;
            fd_set(c, fd, p->fds[fd], fd_cloexec(p, fd));
        }
    }
    c->cwd = p->cwd;
    vnode_get(c->cwd);
    n_clients++;
    return 0;
}

static void close_all(struct client *c, bool only_cloexec)
{
    for (int64_t fd = 0; fd < NOFILE; fd++) {
        if (c->fds[fd] != NULL && (!only_cloexec || fd_cloexec(c, fd))) {
            ofd_put(fd_clear(c, fd));
        }
    }
}

/* INSTALL: new OFDs for procd objects. On failure procd releases the objects itself. */
static int64_t install(struct client *c, const uint64_t a[4], int32_t *fd0, int32_t *fd1)
{
    int32_t *out[2] = { fd0, fd1 };
    for (unsigned i = 0; i < 2 && a[i] != 0; i++) {
        struct ofd *o = slab_alloc(&ofd_cache);
        int fd = -FASTIX_ENOMEM;
        if (o != NULL) {
            uint32_t fl = (uint32_t)a[2 + i];
            *o = (struct ofd){ .refs = 1, .flags = fl & FD_FLAGS, .owner = FASTIX_OWNER_PROCD,
                               .obj = (uint32_t)a[i], .kind = fl >> FASTIX_VFS_KIND_SHIFT };
            fd = fd_install(c, o, 0, (fl & FASTIX_O_CLOEXEC) != 0);
            if (fd < 0) {
                slab_free(&ofd_cache, o);
            }
        }
        if (fd < 0) {
            if (i == 1) {
                slab_free(&ofd_cache, fd_clear(c, *fd0)); /* undo the first FD */
            }
            return fd;
        }
        *out[i] = fd;
    }
    return 0;
}

int64_t client_work(uint32_t kind, uint32_t slot, uint32_t pid, const uint64_t a[4],
                    int32_t *fd0, int32_t *fd1)
{
    if (slot >= FASTIX_VFS_MAX_CLIENTS) {
        return -FASTIX_EINVAL;
    }
    struct client *c = &clients[slot];
    if (kind == FASTIX_VFSW_NEW) {
        return client_new(c, slot, pid, a[0], a[1] != 0);
    }
    if (!c->used || c->pid != pid) {
        return -FASTIX_ESRCH;
    }
    switch (kind) {
    case FASTIX_VFSW_EXEC:
        close_all(c, true);
        return 0;
    case FASTIX_VFSW_EXIT:
        close_all(c, false);
        vnode_put(c->cwd);
        c->used = false;
        n_clients--;
        return 0;
    case FASTIX_VFSW_INSTALL:
        return install(c, a, fd0, fd1);
    default:
        return -FASTIX_EINVAL;
    }
}
