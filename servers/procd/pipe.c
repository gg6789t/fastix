/*
 * pipe.c - SPSC-ring pipes, eventfd2, and temporary Phase 3 FD table (see pipe.h).
 * TEMPORARY: FD table moves to vfsd in Phase 4 (AGENTS.md 6.4).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <arch/vm.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <fastix/pipe_abi.h>
#include <fastix/ring.h>
#include <fastix/signal_abi.h>
#include <libos/assert.h>
#include <libos/list.h>
#include <libos/mem.h>
#include <libos/ring.h>
#include <libos/slab.h>

#include "cspace.h"
#include "frame.h"
#include "heap.h"
#include "pipe.h"
#include "reply.h"
#include "signal.h"

#define PIPE_RING_BYTES  (FASTIX_RING_DATA_OFFSET + FASTIX_PIPE_RING_SIZE)
#define PIPE_RING_PAGES  ((PIPE_RING_BYTES + FASTIX_PAGE_SIZE - 1u) / FASTIX_PAGE_SIZE)
#define PIPE_SLOT_STRIDE (32u * FASTIX_PAGE_SIZE) /* 128 KB VA stride per active pipe */
#define IO_CHUNK_MAX     FASTIX_PIPE_RING_SIZE

enum fdobj_kind {
    FDOBJ_PIPE_RD = 1,
    FDOBJ_PIPE_WR = 2,
    FDOBJ_EVENTFD = 3,
};

struct pipe_obj {
    uint32_t slot;
    uint32_t readers;
    uint32_t writers;
    struct frame *frames[PIPE_RING_PAGES];
    seL4_CPtr map_caps[PIPE_RING_PAGES];
    struct fastix_ring_hdr *ring;
    struct list_node rd_waiters;
    struct list_node wr_waiters;
};

struct eventfd_obj {
    uint64_t counter;
    struct list_node rd_waiters;
    struct list_node wr_waiters;
};

struct fd_obj {
    enum fdobj_kind kind;
    uint32_t refs;
    uint32_t status_flags; /* FASTIX_O_NONBLOCK, FASTIX_EFD_SEMAPHORE */
    union {
        struct pipe_obj *pipe;
        struct eventfd_obj *efd;
    } u;
};

static struct slab_cache fdobj_cache, pipe_cache, efd_cache;
static uint64_t pipe_slot_bits; /* 64 active pipe VA slots */
static uint8_t io_buf[IO_CHUNK_MAX]; /* single-threaded procd scratch copy buffer */

void pipe_module_init(void)
{
    slab_init(&fdobj_cache, "fd_obj", sizeof(struct fd_obj), heap_provider());
    slab_init(&pipe_cache, "pipe_obj", sizeof(struct pipe_obj), heap_provider());
    slab_init(&efd_cache, "eventfd_obj", sizeof(struct eventfd_obj), heap_provider());
}

/* ---- pipe object allocation & destruction ---- */

static uint64_t pipe_slot_vaddr(uint32_t slot, unsigned page_idx)
{
    return FASTIX_PROCD_PIPE_VADDR + (uint64_t)slot * PIPE_SLOT_STRIDE +
           (uint64_t)page_idx * FASTIX_PAGE_SIZE;
}

static void pipe_obj_destroy(struct pipe_obj *po)
{
    for (unsigned i = 0; i < PIPE_RING_PAGES; i++) {
        if (po->map_caps[i] != seL4_CapNull) {
            (void)seL4_CNode_Delete(cspace_root(), po->map_caps[i], seL4_WordBits);
            kobj_slot_put(po->map_caps[i]);
            po->map_caps[i] = seL4_CapNull;
        }
        if (po->frames[i] != NULL) {
            frame_put(po->frames[i]);
            po->frames[i] = NULL;
        }
    }
    pipe_slot_bits &= ~(1ULL << po->slot);
    slab_free(&pipe_cache, po);
}

static int pipe_obj_create(struct pipe_obj **out)
{
    if (~pipe_slot_bits == 0) {
        return -FASTIX_ENFILE;
    }
    uint32_t slot = (uint32_t)__builtin_ctzll(~pipe_slot_bits);
    struct pipe_obj *po = slab_alloc(&pipe_cache);
    if (po == NULL) {
        return -FASTIX_ENOMEM;
    }
    pipe_slot_bits |= 1ULL << slot;
    po->slot = slot;
    po->readers = 1;
    po->writers = 1;
    list_init(&po->rd_waiters);
    list_init(&po->wr_waiters);

    for (unsigned i = 0; i < PIPE_RING_PAGES; i++) {
        int err = frame_new(&po->frames[i]);
        if (err == 0) {
            err = kobj_slot_get(&po->map_caps[i]);
        }
        if (err == 0) {
            err = kobj_sel4_err(seL4_CNode_Copy(cspace_root(), po->map_caps[i], seL4_WordBits,
                                                cspace_root(), po->frames[i]->cap, seL4_WordBits,
                                                seL4_AllRights));
            if (err != 0) {
                kobj_slot_put(po->map_caps[i]);
                po->map_caps[i] = seL4_CapNull;
            }
        }
        if (err == 0) {
            err = vm_map_cap(seL4_CapInitThreadVSpace, po->map_caps[i], pipe_slot_vaddr(slot, i),
                             true, false, heap_keep_pt, NULL);
        }
        if (err != 0) {
            pipe_obj_destroy(po);
            return err;
        }
    }

    po->ring = (struct fastix_ring_hdr *)(uintptr_t)pipe_slot_vaddr(slot, 0);
    int err = ring_init(po->ring, FASTIX_PIPE_RING_SIZE, 1, 0);
    libos_assert(err == 0);
    *out = po;
    return 0;
}

/* ---- wakeup helpers ---- */

static void reply_io(struct thread *t, int64_t ret)
{
    struct reply_obj *rep = t->park_reply;
    t->park_reply = NULL;
    t->park_kind = PARK_NONE;
    t->io_obj = NULL;
    uint64_t mr[1] = { (uint64_t)ret };
    reply_send(rep, 1, mr);
}

static void pipe_send_sigpipe(struct thread *t)
{
    struct fastix_siginfo info = { .si_signo = FASTIX_SIGPIPE, .si_code = FASTIX_SI_USER };
    (void)sig_send_thread(t, FASTIX_SIGPIPE, &info);
}

static void pipe_wake_readers(struct pipe_obj *po)
{
    struct list_node *n, *tmp;
    LIST_FOREACH_SAFE(n, tmp, &po->rd_waiters) {
        struct thread *t = LIST_ENTRY(n, struct thread, io_link);
        size_t chunk = t->io_len < IO_CHUNK_MAX ? (size_t)t->io_len : IO_CHUNK_MAX;
        bool need_notify = false;
        int64_t got = ring_read(po->ring, io_buf, chunk, &need_notify);
        if (got == -FASTIX_EAGAIN) {
            break; /* ring empty again, still open */
        }
        list_remove(n);
        if (got > 0) {
            int werr = vms_write_user(t->proc->vs, t->io_buf_u, io_buf, (size_t)got);
            reply_io(t, werr != 0 ? werr : got);
        } else {
            reply_io(t, 0); /* EOF */
        }
    }
}

static void pipe_wake_writers(struct pipe_obj *po)
{
    struct list_node *n, *tmp;
    LIST_FOREACH_SAFE(n, tmp, &po->wr_waiters) {
        struct thread *t = LIST_ENTRY(n, struct thread, io_link);
        if (po->readers == 0) {
            list_remove(n);
            struct proc *p = t->proc;
            int64_t ret = t->io_done > 0 ? (int64_t)t->io_done : -FASTIX_EPIPE;
            reply_io(t, ret);
            if (!p->zombie && !t->exited) {
                pipe_send_sigpipe(t);
            }
            continue;
        }
        uint64_t rem = t->io_len - t->io_done;
        bool atomic_all = t->io_len <= FASTIX_PIPE_BUF;
        uint32_t avail = ring_free(po->ring);
        if (avail == 0 || (atomic_all && avail < rem)) {
            continue;
        }
        size_t chunk = rem < IO_CHUNK_MAX ? (size_t)rem : IO_CHUNK_MAX;
        if (chunk > avail) {
            chunk = avail;
        }
        int rerr = vms_read(t->proc->vs, t->io_buf_u + t->io_done, io_buf, chunk);
        if (rerr != 0) {
            list_remove(n);
            reply_io(t, t->io_done > 0 ? (int64_t)t->io_done : rerr);
            continue;
        }
        bool need_notify = false;
        int64_t wrote = ring_write(po->ring, io_buf, chunk, atomic_all, &need_notify);
        if (wrote > 0) {
            t->io_done += (uint64_t)wrote;
            pipe_wake_readers(po);
            if (t->io_done >= t->io_len) {
                list_remove(n);
                reply_io(t, (int64_t)t->io_done);
            }
        }
    }
}

static void efd_wake_readers(struct eventfd_obj *efd)
{
    struct list_node *n, *tmp;
    LIST_FOREACH_SAFE(n, tmp, &efd->rd_waiters) {
        if (efd->counter == 0) {
            break;
        }
        struct thread *t = LIST_ENTRY(n, struct thread, io_link);
        struct fd_obj *obj = t->io_obj;
        uint64_t val = (obj->status_flags & FASTIX_EFD_SEMAPHORE) ? 1ULL : efd->counter;
        efd->counter -= val;
        list_remove(n);
        int err = vms_write_user(t->proc->vs, t->io_buf_u, &val, sizeof(val));
        reply_io(t, err != 0 ? err : (int64_t)sizeof(val));
    }
}

static void efd_wake_writers(struct eventfd_obj *efd)
{
    struct list_node *n, *tmp;
    LIST_FOREACH_SAFE(n, tmp, &efd->wr_waiters) {
        struct thread *t = LIST_ENTRY(n, struct thread, io_link);
        uint64_t add = t->io_efd_val;
        if (add > (UINT64_MAX - 1ULL) - efd->counter) {
            continue;
        }
        efd->counter += add;
        list_remove(n);
        reply_io(t, (int64_t)sizeof(add));
    }
    if (efd->counter > 0) {
        efd_wake_readers(efd);
    }
}

/* ---- OFD refcounting & temporary FD table ---- */

static void fdobj_get(struct fd_obj *o)
{
    o->refs++;
    if (o->kind == FDOBJ_PIPE_RD) {
        o->u.pipe->readers++;
    } else if (o->kind == FDOBJ_PIPE_WR) {
        o->u.pipe->writers++;
    }
}

static void fdobj_put(struct fd_obj *o)
{
    if (o == NULL) {
        return;
    }
    libos_assert(o->refs > 0);
    if (o->kind == FDOBJ_PIPE_RD) {
        struct pipe_obj *po = o->u.pipe;
        po->readers--;
        if (po->readers == 0) {
            bool notify = false;
            ring_close_read(po->ring, &notify);
            pipe_wake_writers(po);
        }
        if (--o->refs == 0) {
            if (po->readers == 0 && po->writers == 0) {
                pipe_obj_destroy(po);
            }
            slab_free(&fdobj_cache, o);
        }
    } else if (o->kind == FDOBJ_PIPE_WR) {
        struct pipe_obj *po = o->u.pipe;
        po->writers--;
        if (po->writers == 0) {
            bool notify = false;
            ring_close_write(po->ring, &notify);
            pipe_wake_readers(po);
        }
        if (--o->refs == 0) {
            if (po->readers == 0 && po->writers == 0) {
                pipe_obj_destroy(po);
            }
            slab_free(&fdobj_cache, o);
        }
    } else {
        if (--o->refs == 0) {
            slab_free(&efd_cache, o->u.efd);
            slab_free(&fdobj_cache, o);
        }
    }
}

static int fd_alloc_from(struct proc *p, int32_t min_fd)
{
    if (min_fd < 0 || min_fd >= PROCD_TEMP_MAX_FDS) {
        return -FASTIX_EMFILE;
    }
    for (int32_t i = min_fd; i < PROCD_TEMP_MAX_FDS; i++) {
        if (p->temp_fds[i].obj == NULL) {
            return i;
        }
    }
    return -FASTIX_EMFILE;
}

static struct fd_obj *fd_lookup(struct proc *p, int32_t fd)
{
    if (fd < 0 || fd >= PROCD_TEMP_MAX_FDS) {
        return NULL;
    }
    return p->temp_fds[fd].obj;
}

void pipe_proc_init(struct proc *p, const struct proc *parent)
{
    for (int i = 0; i < PROCD_TEMP_MAX_FDS; i++) {
        if (parent != NULL && parent->temp_fds[i].obj != NULL) {
            p->temp_fds[i] = parent->temp_fds[i];
            fdobj_get(p->temp_fds[i].obj);
        } else {
            p->temp_fds[i] = (struct temp_fd){ 0 };
        }
    }
}

void pipe_proc_exec(struct proc *p)
{
    for (int i = 0; i < PROCD_TEMP_MAX_FDS; i++) {
        if (p->temp_fds[i].obj != NULL && (p->temp_fds[i].fd_flags & FASTIX_FD_CLOEXEC) != 0) {
            struct fd_obj *o = p->temp_fds[i].obj;
            p->temp_fds[i] = (struct temp_fd){ 0 };
            fdobj_put(o);
        }
    }
}

void pipe_proc_cleanup(struct proc *p)
{
    for (int i = 0; i < PROCD_TEMP_MAX_FDS; i++) {
        if (p->temp_fds[i].obj != NULL) {
            struct fd_obj *o = p->temp_fds[i].obj;
            p->temp_fds[i] = (struct temp_fd){ 0 };
            fdobj_put(o);
        }
    }
}

void pipe_cancel_wait(struct thread *t)
{
    if (t->park_kind == PARK_PIPE_READ || t->park_kind == PARK_PIPE_WRITE ||
        t->park_kind == PARK_EFD_READ || t->park_kind == PARK_EFD_WRITE) {
        list_remove(&t->io_link);
        t->io_obj = NULL;
    }
}

/* ---- syscalls ---- */

int pipe_sys_pipe2(struct proc *p, uint64_t fds_u, uint32_t flags, int32_t out_fds[2])
{
    if ((flags & ~(FASTIX_O_NONBLOCK | FASTIX_O_CLOEXEC)) != 0) {
        return -FASTIX_EINVAL;
    }
    int fd0 = fd_alloc_from(p, 0);
    if (fd0 < 0) {
        return fd0;
    }
    int fd1 = fd_alloc_from(p, fd0 + 1);
    if (fd1 < 0) {
        return fd1;
    }
    struct pipe_obj *po;
    int err = pipe_obj_create(&po);
    if (err != 0) {
        return err;
    }
    struct fd_obj *rd = slab_alloc(&fdobj_cache);
    struct fd_obj *wr = slab_alloc(&fdobj_cache);
    if (rd == NULL || wr == NULL) {
        if (rd != NULL) {
            slab_free(&fdobj_cache, rd);
        }
        if (wr != NULL) {
            slab_free(&fdobj_cache, wr);
        }
        pipe_obj_destroy(po);
        return -FASTIX_ENOMEM;
    }
    uint32_t sflags = flags & FASTIX_O_NONBLOCK;
    uint32_t fflags = (flags & FASTIX_O_CLOEXEC) ? FASTIX_FD_CLOEXEC : 0;
    *rd = (struct fd_obj){ .kind = FDOBJ_PIPE_RD, .refs = 1, .status_flags = sflags, .u.pipe = po };
    *wr = (struct fd_obj){ .kind = FDOBJ_PIPE_WR, .refs = 1, .status_flags = sflags, .u.pipe = po };

    int32_t fds[2] = { fd0, fd1 };
    if (fds_u != 0) {
        err = vms_write_user(p->vs, fds_u, fds, sizeof(fds));
        if (err != 0) {
            slab_free(&fdobj_cache, rd);
            slab_free(&fdobj_cache, wr);
            pipe_obj_destroy(po);
            return err;
        }
    }
    p->temp_fds[fd0] = (struct temp_fd){ .obj = rd, .fd_flags = fflags };
    p->temp_fds[fd1] = (struct temp_fd){ .obj = wr, .fd_flags = fflags };
    out_fds[0] = fd0;
    out_fds[1] = fd1;
    return 0;
}

int pipe_sys_eventfd2(struct proc *p, uint32_t initval, uint32_t flags)
{
    uint32_t allowed = FASTIX_EFD_SEMAPHORE | FASTIX_EFD_NONBLOCK | FASTIX_EFD_CLOEXEC;
    if ((flags & ~allowed) != 0) {
        return -FASTIX_EINVAL;
    }
    int fd = fd_alloc_from(p, 0);
    if (fd < 0) {
        return fd;
    }
    struct eventfd_obj *efd = slab_alloc(&efd_cache);
    struct fd_obj *obj = slab_alloc(&fdobj_cache);
    if (efd == NULL || obj == NULL) {
        if (efd != NULL) {
            slab_free(&efd_cache, efd);
        }
        if (obj != NULL) {
            slab_free(&fdobj_cache, obj);
        }
        return -FASTIX_ENOMEM;
    }
    efd->counter = initval;
    list_init(&efd->rd_waiters);
    list_init(&efd->wr_waiters);

    uint32_t sflags = flags & (FASTIX_EFD_NONBLOCK | FASTIX_EFD_SEMAPHORE);
    uint32_t fflags = (flags & FASTIX_EFD_CLOEXEC) ? FASTIX_FD_CLOEXEC : 0;
    *obj = (struct fd_obj){ .kind = FDOBJ_EVENTFD, .refs = 1, .status_flags = sflags, .u.efd = efd };
    p->temp_fds[fd] = (struct temp_fd){ .obj = obj, .fd_flags = fflags };
    return fd;
}

int pipe_sys_close(struct proc *p, int32_t fd)
{
    struct fd_obj *o = fd_lookup(p, fd);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    p->temp_fds[fd] = (struct temp_fd){ 0 };
    fdobj_put(o);
    return 0;
}

int pipe_sys_dup(struct proc *p, int32_t oldfd)
{
    struct fd_obj *o = fd_lookup(p, oldfd);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    int newfd = fd_alloc_from(p, 0);
    if (newfd < 0) {
        return newfd;
    }
    fdobj_get(o);
    p->temp_fds[newfd] = (struct temp_fd){ .obj = o, .fd_flags = 0 };
    return newfd;
}

int pipe_sys_dup3(struct proc *p, int32_t oldfd, int32_t newfd, uint32_t flags)
{
    if ((flags & ~FASTIX_O_CLOEXEC) != 0 || oldfd == newfd) {
        return -FASTIX_EINVAL;
    }
    if (newfd < 0 || newfd >= PROCD_TEMP_MAX_FDS) {
        return -FASTIX_EBADF;
    }
    struct fd_obj *o = fd_lookup(p, oldfd);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    if (p->temp_fds[newfd].obj != NULL) {
        struct fd_obj *prev = p->temp_fds[newfd].obj;
        p->temp_fds[newfd] = (struct temp_fd){ 0 };
        fdobj_put(prev);
    }
    fdobj_get(o);
    p->temp_fds[newfd] = (struct temp_fd){
        .obj = o,
        .fd_flags = (flags & FASTIX_O_CLOEXEC) ? FASTIX_FD_CLOEXEC : 0,
    };
    return newfd;
}

int pipe_sys_fcntl(struct proc *p, int32_t fd, uint32_t cmd, uint64_t arg)
{
    struct fd_obj *o = fd_lookup(p, fd);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    switch (cmd) {
    case FASTIX_F_GETFD:
        return (int)p->temp_fds[fd].fd_flags;
    case FASTIX_F_SETFD:
        p->temp_fds[fd].fd_flags = ((uint32_t)arg & FASTIX_FD_CLOEXEC);
        return 0;
    case FASTIX_F_GETFL:
        return (int)(o->status_flags & FASTIX_O_NONBLOCK);
    case FASTIX_F_SETFL:
        o->status_flags = (o->status_flags & ~FASTIX_O_NONBLOCK) |
                          ((uint32_t)arg & FASTIX_O_NONBLOCK);
        return 0;
    case FASTIX_F_DUPFD:
    case FASTIX_F_DUPFD_CLOEXEC: {
        int nfd = fd_alloc_from(p, (int32_t)arg);
        if (nfd < 0) {
            return nfd;
        }
        fdobj_get(o);
        p->temp_fds[nfd] = (struct temp_fd){
            .obj = o,
            .fd_flags = (cmd == FASTIX_F_DUPFD_CLOEXEC) ? FASTIX_FD_CLOEXEC : 0,
        };
        return nfd;
    }
    default:
        return -FASTIX_EINVAL;
    }
}

int64_t pipe_sys_read(struct thread *t, int32_t fd, uint64_t buf_u, uint64_t len,
                      enum park_kind *park_out)
{
    *park_out = PARK_NONE;
    struct fd_obj *o = fd_lookup(t->proc, fd);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    bool nonblock = (o->status_flags & FASTIX_O_NONBLOCK) != 0;

    if (o->kind == FDOBJ_PIPE_RD) {
        if (len == 0) {
            return 0;
        }
        struct pipe_obj *po = o->u.pipe;
        size_t chunk = len < IO_CHUNK_MAX ? (size_t)len : IO_CHUNK_MAX;
        bool need_notify = false;
        int64_t got = ring_read(po->ring, io_buf, chunk, &need_notify);
        if (got == -FASTIX_EAGAIN) {
            if (nonblock) {
                return -FASTIX_EAGAIN;
            }
            t->io_obj = o;
            t->io_buf_u = buf_u;
            t->io_len = len;
            t->io_done = 0;
            list_push_back(&po->rd_waiters, &t->io_link);
            *park_out = PARK_PIPE_READ;
            return 0;
        }
        if (got > 0) {
            int err = vms_write_user(t->proc->vs, buf_u, io_buf, (size_t)got);
            pipe_wake_writers(po);
            return err != 0 ? err : got;
        }
        return 0; /* EOF */
    }
    if (o->kind == FDOBJ_EVENTFD) {
        if (len < sizeof(uint64_t)) {
            return -FASTIX_EINVAL;
        }
        struct eventfd_obj *efd = o->u.efd;
        if (efd->counter == 0) {
            if (nonblock) {
                return -FASTIX_EAGAIN;
            }
            t->io_obj = o;
            t->io_buf_u = buf_u;
            t->io_len = len;
            t->io_done = 0;
            list_push_back(&efd->rd_waiters, &t->io_link);
            *park_out = PARK_EFD_READ;
            return 0;
        }
        uint64_t val = (o->status_flags & FASTIX_EFD_SEMAPHORE) ? 1ULL : efd->counter;
        efd->counter -= val;
        int err = vms_write_user(t->proc->vs, buf_u, &val, sizeof(val));
        efd_wake_writers(efd);
        return err != 0 ? err : (int64_t)sizeof(val);
    }
    return -FASTIX_EBADF; /* writing-only pipe end */
}

int64_t pipe_sys_write(struct thread *t, int32_t fd, uint64_t buf_u, uint64_t len,
                       enum park_kind *park_out)
{
    *park_out = PARK_NONE;
    struct fd_obj *o = fd_lookup(t->proc, fd);
    if (o == NULL) {
        return -FASTIX_EBADF;
    }
    bool nonblock = (o->status_flags & FASTIX_O_NONBLOCK) != 0;

    if (o->kind == FDOBJ_PIPE_WR) {
        struct pipe_obj *po = o->u.pipe;
        if (po->readers == 0) {
            pipe_send_sigpipe(t);
            return -FASTIX_EPIPE;
        }
        if (len == 0) {
            return 0;
        }
        bool atomic_all = len <= FASTIX_PIPE_BUF;
        uint64_t done = 0;
        while (done < len) {
            uint64_t rem = len - done;
            uint32_t avail = ring_free(po->ring);
            if (avail == 0 || (atomic_all && avail < rem)) {
                break;
            }
            size_t chunk = rem < IO_CHUNK_MAX ? (size_t)rem : IO_CHUNK_MAX;
            if (chunk > avail) {
                chunk = avail;
            }
            int err = vms_read(t->proc->vs, buf_u + done, io_buf, chunk);
            if (err != 0) {
                return done > 0 ? (int64_t)done : err;
            }
            bool need_notify = false;
            int64_t wrote = ring_write(po->ring, io_buf, chunk, atomic_all, &need_notify);
            if (wrote < 0) {
                pipe_send_sigpipe(t);
                return done > 0 ? (int64_t)done : wrote;
            }
            if (wrote == 0) {
                break;
            }
            done += (uint64_t)wrote;
            pipe_wake_readers(po);
            if (nonblock) {
                break; /* non-blocking write returns what fit immediately */
            }
        }
        if (done == len || (nonblock && done > 0)) {
            return (int64_t)done;
        }
        if (nonblock) {
            return -FASTIX_EAGAIN;
        }
        t->io_obj = o;
        t->io_buf_u = buf_u;
        t->io_len = len;
        t->io_done = done;
        list_push_back(&po->wr_waiters, &t->io_link);
        *park_out = PARK_PIPE_WRITE;
        return 0;
    }
    if (o->kind == FDOBJ_EVENTFD) {
        if (len < sizeof(uint64_t)) {
            return -FASTIX_EINVAL;
        }
        uint64_t val = 0;
        int err = vms_read(t->proc->vs, buf_u, &val, sizeof(val));
        if (err != 0) {
            return err;
        }
        if (val == UINT64_MAX) {
            return -FASTIX_EINVAL;
        }
        struct eventfd_obj *efd = o->u.efd;
        if (val > (UINT64_MAX - 1ULL) - efd->counter) {
            if (nonblock) {
                return -FASTIX_EAGAIN;
            }
            t->io_obj = o;
            t->io_efd_val = val;
            t->io_done = 0;
            list_push_back(&efd->wr_waiters, &t->io_link);
            *park_out = PARK_EFD_WRITE;
            return 0;
        }
        efd->counter += val;
        efd_wake_readers(efd);
        return (int64_t)sizeof(val);
    }
    return -FASTIX_EBADF; /* read-only pipe end */
}
