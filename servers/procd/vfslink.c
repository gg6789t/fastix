/* vfslink.c - procd's side of the vfsd link (see vfslink.h, docs/adr/0003-vfsd-core.md). */
#include <stdbool.h>
#include <stdint.h>

#include <arch/regs.h>
#include <fastix/config.h>
#include <fastix/cspace.h>
#include <fastix/dev_proto.h>
#include <fastix/errno.h>
#include <fastix/mman.h>
#include <fastix/signal_abi.h>
#include <fastix/vfs_proto.h>
#include <fastix/vspace.h>
#include <libos/kprint.h>
#include <libos/list.h>
#include <libos/slab.h>

#include "cspace.h"
#include "heap.h"
#include "pipe.h"
#include "proc.h"
#include "proc_internal.h"
#include "server.h"
#include "vfslink.h"

extern const uint8_t fastix_image_vfsd_start[], fastix_image_vfsd_end[];

_Static_assert(FASTIX_FDEXP_PAGES == 1, "vfslink maps exactly one FD export page per client");
_Static_assert(VFSLINK_NO_SLOT == FASTIX_VFS_NO_PARENT, "one 'no slot' value on both sides");

struct vfs_work {
    struct list_node link;
    uint64_t token;
    uint32_t kind;
    uint32_t slot;        /* client slot the item is about */
    uint32_t parent_slot; /* NEW: slot whose FDs are copied, or VFSLINK_NO_SLOT */
    bool cloexec;         /* NEW: leave out the parent's FD_CLOEXEC descriptors */
    uint32_t pid;         /* client process (NEW, EXEC: checked with gen before starting it) */
    uint16_t gen;
    uint32_t waiter_pid;  /* thread parked in PARK_VFS for this item, 0 = none */
    uint32_t waiter_tid;
    uint32_t obj[2];      /* INSTALL */
    uint32_t flags[2];
};

/* procd's record of one vfsd client; lives from vfslink_new until vfsd acknowledges EXIT. */
struct vfs_client {
    bool mapped;              /* window, info page and FD table are mapped in vfsd */
    struct anon_shm *win;     /* the process's I/O windows */
    struct frame *info;       /* the process's procinfo page */
    struct frame *fdexp;      /* exported FD table, written by vfsd */
    seL4_CPtr view;           /* procd's read-only mapping of fdexp */
    struct vfs_work *exit;    /* reserved EXIT item, so exit never fails */
};

static struct proc *vfsd;
static struct kobj service_ep, wake_ntfn, vfsd_reply;
static seL4_CPtr wake_cap; /* procd's badged copy of wake_ntfn */
static struct vfs_client *clients[FASTIX_VFS_MAX_CLIENTS];
static struct slab_cache client_cache, work_cache;
static struct list_node pending;
static struct vfs_work *inflight;
static uint64_t next_token = 1;

static uint64_t view_addr(uint32_t slot)
{
    return FASTIX_PROCD_FDEXP_VADDR + (uint64_t)slot * FASTIX_PAGE_SIZE;
}

static uint64_t client_addr(uint32_t slot, uint64_t off)
{
    return fastix_vfsd_client_addr(slot, off);
}

int vfslink_init(void)
{
    slab_init(&client_cache, "vfs_client", sizeof(struct vfs_client), heap_provider());
    slab_init(&work_cache, "vfs_work", sizeof(struct vfs_work), heap_provider());
    list_init(&pending);
    int err = kobj_alloc(seL4_EndpointObject, 0, &service_ep);
    if (err == 0) {
        err = kobj_alloc(seL4_NotificationObject, 0, &wake_ntfn);
    }
    return err;
}

/* ---- client records ---- */

static void client_free(uint32_t slot)
{
    struct vfs_client *c = clients[slot];
    if (c->mapped) {
        vms_remove_special(vfsd->vs, client_addr(slot, 0), FASTIX_VFSD_CLIENT_STRIDE);
    }
    if (c->view != seL4_CapNull) {
        (void)seL4_CNode_Delete(cspace_root(), c->view, seL4_WordBits); /* unmaps the view */
        kobj_slot_put(c->view);
    }
    if (c->fdexp != NULL) {
        frame_put(c->fdexp);
    }
    if (c->info != NULL) {
        frame_put(c->info);
    }
    vms_shm_put(c->win);
    if (c->exit != NULL) {
        slab_free(&work_cache, c->exit);
    }
    slab_free(&client_cache, c);
    clients[slot] = NULL;
}

/* Gives c a zeroed FD export page and maps it read-only into procd. */
static int client_export(struct vfs_client *c, uint32_t slot)
{
    int err = frame_new(&c->fdexp);
    if (err != 0) {
        return err;
    }
    err = kobj_slot_get(&c->view);
    if (err != 0) {
        c->view = seL4_CapNull;
        return err;
    }
    err = kobj_sel4_err(seL4_CNode_Copy(cspace_root(), c->view, seL4_WordBits, cspace_root(),
                                        c->fdexp->cap, seL4_WordBits, seL4_AllRights));
    if (err != 0) {
        kobj_slot_put(c->view);
        c->view = seL4_CapNull;
        return err;
    }
    return vm_map_cap(seL4_CapInitThreadVSpace, c->view, view_addr(slot), false, false,
                      heap_keep_pt, NULL);
}

/* Lowest free slot: reusing slots keeps vfsd's per-client memory bounded by the number of
 * processes alive at once. */
static int find_slot(uint32_t *out)
{
    for (uint32_t s = 0; s < FASTIX_VFS_MAX_CLIENTS; s++) {
        if (clients[s] == NULL) {
            *out = s;
            return 0;
        }
    }
    return -FASTIX_EAGAIN;
}

/* Creates p's client record and gives p its badged vfsd endpoint cap. */
static int client_new(struct proc *p, uint32_t *slot_out)
{
    uint32_t slot;
    int err = find_slot(&slot);
    if (err != 0) {
        return err;
    }
    struct vfs_client *c = slab_alloc(&client_cache); /* zeroed */
    if (c == NULL) {
        return -FASTIX_ENOMEM;
    }
    clients[slot] = c;
    vms_shm_get(p->iowin);
    c->win = p->iowin;
    frame_get(p->procinfo);
    c->info = p->procinfo;
    c->exit = slab_alloc(&work_cache);
    err = c->exit == NULL ? -FASTIX_ENOMEM : client_export(c, slot);
    if (err == 0) {
        uint64_t badge = fastix_badge_make(FASTIX_BADGE_SYSCALL, p->pid, slot, p->gen);
        err = kobj_sel4_err(seL4_CNode_Mint(p->cnode.cap, FASTIX_CAP_VFSD_EP,
                                            FASTIX_ROOT_CNODE_BITS, cspace_root(), service_ep.cap,
                                            seL4_WordBits, CLIENT_EP_RIGHTS, badge));
    }
    if (err != 0) {
        client_free(slot);
        return err;
    }
    *slot_out = slot;
    return 0;
}

/* Maps a client's pages into vfsd when vfsd fetches its NEW item. */
static int client_map(uint32_t slot)
{
    struct vfs_client *c = clients[slot];
    uint32_t rw = FASTIX_PROT_READ | FASTIX_PROT_WRITE;
    int err = vms_add_special_shm(vfsd->vs, client_addr(slot, FASTIX_VFSD_WIN_OFF),
                                  FASTIX_IOWIN_REGION, rw, c->win);
    if (err == 0) {
        err = vms_add_special(vfsd->vs, client_addr(slot, FASTIX_VFSD_INFO_OFF), c->info,
                              FASTIX_PROT_READ);
    }
    if (err == 0) {
        err = vms_add_special(vfsd->vs, client_addr(slot, FASTIX_VFSD_FDEXP_OFF), c->fdexp, rw);
    }
    if (err != 0) {
        vms_remove_special(vfsd->vs, client_addr(slot, 0), FASTIX_VFSD_CLIENT_STRIDE);
        return err;
    }
    c->mapped = true;
    return 0;
}

/* ---- work queue ---- */

static void wake_vfsd(void)
{
    if (wake_cap != seL4_CapNull) {
        seL4_Signal(wake_cap);
    }
}

static void enqueue(struct vfs_work *w)
{
    w->token = next_token++;
    list_push_back(&pending, &w->link);
    wake_vfsd();
}

static struct vfs_work *work_new(uint32_t kind, const struct proc *p, const struct thread *waiter)
{
    struct vfs_work *w = slab_alloc(&work_cache);
    if (w == NULL) {
        return NULL;
    }
    w->kind = kind;
    w->slot = p->vfs_slot;
    w->parent_slot = VFSLINK_NO_SLOT;
    w->pid = p->pid;
    w->gen = p->gen;
    if (waiter != NULL) {
        w->waiter_pid = waiter->proc->pid;
        w->waiter_tid = waiter->tid;
    }
    return w;
}

int vfslink_new(struct proc *p, struct proc *parent, struct thread *waiter, bool cloexec)
{
    uint32_t slot;
    int err = client_new(p, &slot);
    if (err != 0) {
        return err;
    }
    p->vfs_slot = slot;
    struct vfs_work *w = work_new(FASTIX_VFSW_NEW, p, waiter);
    if (w == NULL) {
        client_free(slot);
        p->vfs_slot = VFSLINK_NO_SLOT;
        return -FASTIX_ENOMEM;
    }
    w->parent_slot = parent != NULL ? parent->vfs_slot : VFSLINK_NO_SLOT;
    w->cloexec = cloexec;
    p->start_pending = true;
    enqueue(w);
    return 0;
}

int vfslink_exec(struct proc *p)
{
    struct vfs_work *w = work_new(FASTIX_VFSW_EXEC, p, NULL);
    if (w == NULL) {
        return -FASTIX_ENOMEM;
    }
    p->start_pending = true;
    enqueue(w);
    return 0;
}

int vfslink_set_window(struct proc *p)
{
    struct vfs_client *c = clients[p->vfs_slot];
    uint64_t addr = client_addr(p->vfs_slot, FASTIX_VFSD_WIN_OFF);
    vms_shm_get(p->iowin);
    vms_shm_put(c->win);
    c->win = p->iowin;
    if (!c->mapped) {
        return 0;
    }
    vms_remove_special(vfsd->vs, addr, FASTIX_IOWIN_REGION);
    return vms_add_special_shm(vfsd->vs, addr, FASTIX_IOWIN_REGION,
                               FASTIX_PROT_READ | FASTIX_PROT_WRITE, c->win);
}

void vfslink_exit(struct proc *p)
{
    if (p->vfs_slot == VFSLINK_NO_SLOT) {
        if (p == vfsd) {
            kprint_str("procd: vfsd exited; files are no longer available\n");
        }
        return;
    }
    struct vfs_client *c = clients[p->vfs_slot];
    struct vfs_work *w = c->exit;
    c->exit = NULL; /* freed when vfsd acknowledges it */
    *w = (struct vfs_work){ .kind = FASTIX_VFSW_EXIT, .slot = p->vfs_slot, .pid = p->pid,
                            .gen = p->gen, .parent_slot = VFSLINK_NO_SLOT };
    p->vfs_slot = VFSLINK_NO_SLOT;
    enqueue(w);
}

int vfslink_install(struct thread *t, unsigned n, const uint32_t obj[2], const uint32_t flags[2],
                    uint64_t fds_u)
{
    struct vfs_work *w = work_new(FASTIX_VFSW_INSTALL, t->proc, t);
    if (w == NULL) {
        return -FASTIX_ENOMEM;
    }
    for (unsigned i = 0; i < n && i < 2; i++) {
        w->obj[i] = obj[i];
        w->flags[i] = flags[i];
    }
    t->vfs_fds_u = fds_u;
    enqueue(w);
    return 0;
}

uint32_t vfslink_fd_word(const struct proc *p, int32_t fd)
{
    if (p->vfs_slot == VFSLINK_NO_SLOT || fd < 0 || fd >= FASTIX_DEFAULT_NOFILE) {
        return 0;
    }
    const uint32_t *tab = (const uint32_t *)(uintptr_t)view_addr(p->vfs_slot);
    return __atomic_load_n(&tab[fd], __ATOMIC_ACQUIRE);
}

/* ---- completing items ---- */

static struct proc *live_proc(uint32_t pid, uint16_t gen)
{
    struct proc *p = proc_get(pid);
    return (p != NULL && !p->zombie && p->gen == gen) ? p : NULL;
}

/* Answers the thread parked in PARK_VFS for `w` (if it still waits). */
static void answer_waiter(const struct vfs_work *w, int64_t val)
{
    struct proc *wp = w->waiter_pid != 0 ? proc_get(w->waiter_pid) : NULL;
    struct thread *t = wp != NULL ? proc_find_thread(wp, w->waiter_tid) : NULL;
    if (t == NULL || t->park_kind != PARK_VFS || t->park_reply == NULL) {
        return;
    }
    struct reply_obj *rep = t->park_reply;
    t->park_reply = NULL;
    t->park_kind = PARK_NONE;
    uint64_t mr[1] = { (uint64_t)val };
    reply_send(rep, 1, mr);
}

static void start_main(struct proc *p)
{
    p->start_pending = false;
    if (proc_thread_start(p->main_thread, &p->start_regs) != 0) {
        proc_exit(p, FASTIX_W_SIGNALED(FASTIX_SIGKILL));
    }
}

static void complete_install(const struct vfs_work *w, int64_t result, int32_t fd0, int32_t fd1)
{
    if (result < 0) {
        for (unsigned i = 0; i < 2; i++) {
            if (w->obj[i] != 0) {
                pipe_obj_release(w->obj[i]);
            }
        }
        answer_waiter(w, result);
        return;
    }
    struct proc *wp = proc_get(w->waiter_pid);
    struct thread *t = wp != NULL ? proc_find_thread(wp, w->waiter_tid) : NULL;
    if (t != NULL && t->vfs_fds_u != 0) {
        int32_t fds[2] = { fd0, fd1 };
        int err = vms_write_user(wp->vs, t->vfs_fds_u, fds, sizeof(fds));
        answer_waiter(w, err);
        return;
    }
    answer_waiter(w, fd0);
}

static void complete(struct vfs_work *w, int64_t result, int32_t fd0, int32_t fd1)
{
    struct proc *p = (w->kind == FASTIX_VFSW_NEW || w->kind == FASTIX_VFSW_EXEC)
                         ? live_proc(w->pid, w->gen)
                         : NULL;
    switch (w->kind) {
    case FASTIX_VFSW_NEW:
        if (p != NULL && result < 0) {
            proc_exit(p, FASTIX_W_SIGNALED(FASTIX_SIGKILL));
        } else if (p != NULL && p->start_pending) {
            start_main(p);
        }
        answer_waiter(w, result < 0 ? result : (int64_t)w->pid);
        break;
    case FASTIX_VFSW_EXEC:
        if (p != NULL && result < 0) {
            proc_exit(p, FASTIX_W_SIGNALED(FASTIX_SIGKILL));
        } else if (p != NULL && p->start_pending) {
            start_main(p);
        }
        break;
    case FASTIX_VFSW_INSTALL:
        complete_install(w, result, fd0, fd1);
        break;
    case FASTIX_VFSW_EXIT:
        client_free(w->slot);
        break;
    default:
        break;
    }
    slab_free(&work_cache, w);
}

/* ---- calls from vfsd ---- */

static void srv_work(unsigned *nret)
{
    /* Read every input first: the seL4 calls below reuse the message registers. */
    uint64_t done = seL4_GetMR(0);
    int64_t result = (int64_t)seL4_GetMR(1);
    int32_t fd0 = (int32_t)seL4_GetMR(2), fd1 = (int32_t)seL4_GetMR(3);

    if (done != 0 && inflight != NULL && inflight->token == done) {
        struct vfs_work *w = inflight;
        inflight = NULL;
        complete(w, result, fd0, fd1);
    }
    struct vfs_work *w = NULL;
    struct list_node *n;
    while (inflight == NULL && (n = list_pop(&pending)) != NULL) {
        w = LIST_ENTRY(n, struct vfs_work, link);
        int err = w->kind == FASTIX_VFSW_NEW ? client_map(w->slot) : 0;
        if (err == 0) {
            inflight = w;
            break;
        }
        complete(w, err, -1, -1);
        w = NULL;
    }
    if (inflight == NULL) {
        seL4_SetMR(0, 0);
        *nret = 1;
        return;
    }
    w = inflight;
    seL4_SetMR(0, w->token);
    seL4_SetMR(1, w->kind);
    seL4_SetMR(2, w->slot);
    seL4_SetMR(3, w->pid);
    bool is_new = w->kind == FASTIX_VFSW_NEW;
    seL4_SetMR(4, is_new ? w->parent_slot : w->obj[0]);
    seL4_SetMR(5, is_new ? (seL4_Word)w->cloexec : w->obj[1]);
    seL4_SetMR(6, w->flags[0]);
    seL4_SetMR(7, w->flags[1]);
    *nret = 8;
}

void vfslink_handle_server_msg(seL4_MessageInfo_t info, unsigned *nret)
{
    int64_t rc = 0;
    switch (seL4_MessageInfo_get_label(info)) {
    case FASTIX_SRV_VFS_WORK:
        srv_work(nret);
        return;
    case FASTIX_SRV_OBJ_RELEASE:
        pipe_obj_release((uint32_t)seL4_GetMR(0));
        break;
    case FASTIX_SRV_OBJ_SETFL:
        pipe_obj_setfl((uint32_t)seL4_GetMR(0), (uint32_t)seL4_GetMR(1));
        break;
    default:
        rc = -FASTIX_ENOSYS;
        break;
    }
    seL4_SetMR(0, (seL4_Word)rc);
    *nret = 1;
}

/* ---- starting vfsd ---- */

/* Gives vfsd its endpoint, its procd server-control cap and its bound wake-up notification. */
static int vfsd_caps(struct proc *p)
{
    uint64_t srv_badge = fastix_badge_make(FASTIX_BADGE_SERVER, FASTIX_SRVID_VFSD, 0, 0);
    uint64_t wake_badge = fastix_badge_make(FASTIX_BADGE_NOTIFY, 0, 0, 0);
    int err = kobj_alloc(seL4_ReplyObject, 0, &vfsd_reply);
    if (err != 0) {
        return err;
    }
    err = kobj_slot_get(&wake_cap);
    if (err != 0) {
        wake_cap = seL4_CapNull;
        return err;
    }
    seL4_Error e = seL4_CNode_Copy(p->cnode.cap, FASTIX_CAP_SERVER_EP, FASTIX_ROOT_CNODE_BITS,
                                   cspace_root(), service_ep.cap, seL4_WordBits, seL4_AllRights);
    if (e == seL4_NoError) {
        e = seL4_CNode_Copy(p->cnode.cap, FASTIX_CAP_SERVER_REPLY, FASTIX_ROOT_CNODE_BITS,
                            cspace_root(), vfsd_reply.cap, seL4_WordBits, seL4_AllRights);
    }
    if (e == seL4_NoError) {
        e = seL4_CNode_Mint(p->cnode.cap, FASTIX_CAP_SERVER_PROCD, FASTIX_ROOT_CNODE_BITS,
                            cspace_root(), server_endpoint(), seL4_WordBits, CLIENT_EP_RIGHTS,
                            srv_badge);
    }
    if (e == seL4_NoError) {
        e = seL4_CNode_Mint(cspace_root(), wake_cap, seL4_WordBits, cspace_root(), wake_ntfn.cap,
                            seL4_WordBits, seL4_CanWrite, wake_badge);
    }
    if (e == seL4_NoError) {
        e = seL4_TCB_BindNotification(p->main_thread->tcb.cap, wake_ntfn.cap);
    }
    if (e == seL4_NoError) {
        e = seL4_TCB_SetPriority(p->main_thread->tcb.cap, seL4_CapInitThreadTCB,
                                 FASTIX_VFSD_PRIO);
    }
    return kobj_sel4_err(e);
}

int vfslink_start(void)
{
    const char *argv[] = { "vfsd", NULL };
    const char *envp[] = { NULL };
    struct load_args la = {
        .elf = fastix_image_vfsd_start,
        .elf_size = (size_t)(fastix_image_vfsd_end - fastix_image_vfsd_start),
        .argv = argv,
        .envp = envp,
        .vdso_ehdr = FASTIX_VDSO_CODE_ADDR,
    };
    struct proc *p;
    uint64_t entry = 0, sp = 0;
    int err = proc_create_empty(NULL, 0, &p);
    if (err != 0) {
        return err;
    }
    p->is_server = true;
    if ((err = loader_load(p->vs, &la, &entry, &sp)) == 0 && (err = vfsd_caps(p)) == 0) {
        vfsd = p;
        arch_regs_init_entry(&p->start_regs, entry, sp, 0);
        err = proc_thread_start(p->main_thread, &p->start_regs);
    }
    if (err != 0) {
        vfsd = NULL;
        proc_discard(p);
    }
    return err;
}
