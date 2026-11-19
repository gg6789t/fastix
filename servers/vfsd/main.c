/*
 * main.c - vfsd entry and event loop: sets up memory, the root tmpfs and the client table,
 * then answers client calls and pulls procd work items when procd signals.
 */
#include <fastix/abi.h>
#include <fastix/cspace.h>
#include <fastix/errno.h>
#include <fastix/mman.h>
#include <fastix_sys/sys.h>
#include <libos/kprint.h>

#include "client.h"
#include "fsops.h"
#include "tmpfs.h"
#include "vfsd.h"

#define HEAP_BYTES     (1ULL << 30)  /* address space reserved for slab pages (demand-paged) */
#define ROOT_FS_BYTES  (64ULL << 20) /* data limit of the root tmpfs */

static uint8_t *heap_next, *heap_end;

static int heap_get_page(void *ctx, void **page)
{
    (void)ctx;
    if (heap_next == heap_end) {
        return -FASTIX_ENOMEM;
    }
    *page = heap_next; /* fresh anonymous memory is already zero */
    heap_next += FASTIX_PAGE_SIZE;
    return 0;
}

struct slab_provider vfs_page_provider(void)
{
    return (struct slab_provider){ heap_get_page, NULL };
}

void vfs_now(struct fastix_timespec *ts)
{
    uint64_t ns = fastix_clock_realtime_ns();
    ts->tv_sec = (int64_t)(ns / FASTIX_NSEC_PER_SEC);
    ts->tv_nsec = (int64_t)(ns % FASTIX_NSEC_PER_SEC);
}

/* ---- client memory ---- */

void *uptr(uint32_t slot, uint64_t uaddr, size_t len)
{
    if (uaddr < FASTIX_IOWIN_BASE || len > FASTIX_IOWIN_REGION ||
        uaddr - FASTIX_IOWIN_BASE > FASTIX_IOWIN_REGION - len) {
        return NULL;
    }
    return (void *)(uintptr_t)(client_view(slot, FASTIX_VFSD_WIN_OFF) + (uaddr - FASTIX_IOWIN_BASE));
}

int ustr(uint32_t slot, uint64_t uaddr, char *dst, size_t cap)
{
    if (uaddr < FASTIX_IOWIN_BASE || uaddr - FASTIX_IOWIN_BASE >= FASTIX_IOWIN_REGION) {
        return -FASTIX_EFAULT;
    }
    uint64_t avail = FASTIX_IOWIN_REGION - (uaddr - FASTIX_IOWIN_BASE);
    const char *src = uptr(slot, uaddr, 1);
    for (size_t i = 0; i < cap; i++) {
        if (i >= avail) {
            return -FASTIX_EFAULT;
        }
        dst[i] = src[i];
        if (src[i] == '\0') {
            return 0;
        }
    }
    return -FASTIX_ENAMETOOLONG;
}

/* ---- procd ---- */

int64_t procd_call(uint64_t label, unsigned n, const uint64_t *mr)
{
    for (unsigned i = 0; i < n; i++) {
        seL4_SetMR((int)i, mr[i]);
    }
    (void)seL4_Call(fastix_cptr_l1(FASTIX_CAP_SERVER_PROCD), seL4_MessageInfo_new(label, 0, 0, n));
    return (int64_t)seL4_GetMR(0);
}

void work_drain(void)
{
    uint64_t done = 0;
    int64_t result = 0;
    int32_t fd0 = -1, fd1 = -1;
    for (;;) {
        uint64_t mr[4] = { done, (uint64_t)result, (uint64_t)(int64_t)fd0, (uint64_t)(int64_t)fd1 };
        uint64_t token = (uint64_t)procd_call(FASTIX_SRV_VFS_WORK, 4, mr);
        if (token == 0) {
            return;
        }
        /* Copy the item out first: client_work may call procd and reuse the registers. */
        uint32_t kind = (uint32_t)seL4_GetMR(1), slot = (uint32_t)seL4_GetMR(2);
        uint32_t pid = (uint32_t)seL4_GetMR(3);
        uint64_t a[4] = { seL4_GetMR(4), seL4_GetMR(5), seL4_GetMR(6), seL4_GetMR(7) };
        fd0 = -1;
        fd1 = -1;
        result = client_work(kind, slot, pid, a, &fd0, &fd1);
        done = token;
    }
}

/* ---- calls from clients ---- */

static int64_t test_stats(void)
{
    work_drain();
    struct tmpfs_stats ts = tmpfs_stats();
    seL4_SetMR(1, client_count());
    seL4_SetMR(2, ofd_count());
    seL4_SetMR(3, ts.nodes);
    seL4_SetMR(4, ts.pages);
    return 0;
}

static int64_t dispatch(struct client *c, uint64_t label, const uint64_t a[6], uint32_t *owner)
{
    switch (label) {
    case FASTIX_SYS_openat:     return fs_openat(c, a);
    case FASTIX_SYS_close:      return sys_close(c, (int64_t)a[0]);
    case FASTIX_SYS_read:       return fs_read(c, a, owner);
    case FASTIX_SYS_write:      return fs_write(c, a, owner);
    case FASTIX_SYS_pread64:    return fs_pread64(c, a);
    case FASTIX_SYS_pwrite64:   return fs_pwrite64(c, a);
    case FASTIX_SYS_lseek:      return fs_lseek(c, a);
    case FASTIX_SYS_dup:        return sys_dup(c, (int64_t)a[0]);
    case FASTIX_SYS_dup3:       return sys_dup3(c, (int64_t)a[0], (int64_t)a[1], a[2]);
    case FASTIX_SYS_fcntl:      return sys_fcntl(c, (int64_t)a[0], a[1], a[2]);
    case FASTIX_SYS_fstat:      return fs_fstat(c, a);
    case FASTIX_SYS_newfstatat: return fs_newfstatat(c, a);
    case FASTIX_SYS_getdents64: return fs_getdents64(c, a);
    case FASTIX_SYS_ftruncate:  return fs_ftruncate(c, a);
    case FASTIX_SYS_truncate:   return fs_truncate(c, a);
    case FASTIX_SYS_fchmod:     return fs_fchmod(c, a);
    case FASTIX_SYS_fchmodat:   return fs_fchmodat(c, a);
    case FASTIX_SYS_faccessat:  return fs_faccessat(c, a);
    case FASTIX_SYS_mkdirat:    return fs_mkdirat(c, a);
    case FASTIX_SYS_unlinkat:   return fs_unlinkat(c, a);
    case FASTIX_SYS_symlinkat:  return fs_symlinkat(c, a);
    case FASTIX_SYS_readlinkat: return fs_readlinkat(c, a);
    case FASTIX_SYS_linkat:     return fs_linkat(c, a);
    case FASTIX_SYS_renameat2:  return fs_renameat2(c, a);
    case FASTIX_SYS_chdir:      return fs_chdir(c, a);
    case FASTIX_SYS_fchdir:     return fs_fchdir(c, a);
    case FASTIX_SYS_getcwd:     return fs_getcwd(c, a);
    case FASTIX_SYS_mount:      return fs_mount(c, a);
    case FASTIX_SYS_umount2:    return fs_umount2(c, a);
#ifdef FASTIX_TEST_HOOKS
    case FASTIX_VFS_TEST_STATS: return test_stats();
#endif
    default:                    return -FASTIX_ENOSYS;
    }
}

/* Answers one client call; returns the number of reply words (in the IPC buffer). */
static unsigned handle(uint64_t badge, seL4_MessageInfo_t info)
{
    uint64_t a[6] = { 0 };
    unsigned n = (unsigned)seL4_MessageInfo_get_length(info);
    for (unsigned i = 0; i < n && i < 6; i++) {
        a[i] = seL4_GetMR((int)i);
    }
    struct client *c = client_from_badge(badge);
    uint32_t owner = 0;
    int64_t r = c != NULL ? dispatch(c, seL4_MessageInfo_get_label(info), a, &owner)
                          : -FASTIX_ESRCH;
    seL4_SetMR(0, (uint64_t)r);
    if (r == -FASTIX_ESTALEFD) {
        seL4_SetMR(1, owner);
        return 2;
    }
#ifdef FASTIX_TEST_HOOKS
    if (seL4_MessageInfo_get_label(info) == FASTIX_VFS_TEST_STATS) {
        return 5;
    }
#endif
    return 1;
}

static int setup(void)
{
    int64_t heap = fastix_sys6(FASTIX_SYS_mmap, 0, HEAP_BYTES,
                               FASTIX_PROT_READ | FASTIX_PROT_WRITE,
                               FASTIX_MAP_PRIVATE | FASTIX_MAP_ANONYMOUS, (uint64_t)-1, 0);
    if (heap < 0) {
        return (int)heap;
    }
    heap_next = (uint8_t *)(uintptr_t)heap;
    heap_end = heap_next + HEAP_BYTES;
    tmpfs_module_init();
    struct cred root_cred = { 0, 0 };
    struct vnode *root;
    struct mount *m;
    int err = tmpfs_new(ROOT_FS_BYTES, 0755u, &root_cred, &root);
    if (err == 0) {
        err = vfs_mount_root(root, &m);
    }
    if (err == 0) {
        err = clients_init(root);
    }
    return err;
}

_Noreturn void vfsd_main(void);

_Noreturn void vfsd_main(void)
{
    fastix_sys_init();
    if (setup() != 0) {
        kprint_str("vfsd: setup failed\n");
        (void)fastix_sys1(FASTIX_SYS_exit_group, 1);
    }
    work_drain();
    seL4_CPtr ep = fastix_cptr_l1(FASTIX_CAP_SERVER_EP);
    seL4_CPtr reply = fastix_cptr_l1(FASTIX_CAP_SERVER_REPLY);
    seL4_Word badge = 0;
    seL4_MessageInfo_t info = seL4_Recv(ep, &badge, reply);
    for (;;) {
        if (fastix_badge_type(badge) == FASTIX_BADGE_NOTIFY) {
            work_drain();
            info = seL4_Recv(ep, &badge, reply);
            continue;
        }
        unsigned nret = handle(badge, info);
        info = seL4_ReplyRecv(ep, seL4_MessageInfo_new(0, 0, 0, nret), &badge, reply);
    }
}
