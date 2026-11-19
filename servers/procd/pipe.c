/*
 * pipe.c - SPSC-ring pipes and eventfd2 (see pipe.h). vfsd owns the FD numbers; procd owns
 * one numbered object per open file description (each pipe end, each eventfd).
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
#include <fastix/fs_abi.h>
#include <fastix/vfs_proto.h>
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
#include "vfslink.h"

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
    uint32_t readers; /* read ends not yet released */
    uint32_t writers; /* write ends not yet released */
    uint32_t objs;    /* fd_objs still alive (the pipe is freed when this reaches 0) */
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

/* One open file description: alive until vfsd releases it and no thread waits on it. */
struct fd_obj {
    enum fdobj_kind kind;
    uint32_t index;        /* objs[] index vfsd refers to (0 once released) */
    bool released;         /* vfsd dropped its last reference */
    uint32_t waiting;      /* threads parked on this object */
    uint32_t status_flags; /* FASTIX_O_NONBLOCK, FASTIX_EFD_SEMAPHORE */
    union {
        struct pipe_obj *pipe;
        struct eventfd_obj *efd;
    } u;
};

static struct slab_cache fdobj_cache, pipe_cache, efd_cache;
static struct fd_obj *objs[FASTIX_PROCD_MAX_FDOBJS]; /* index 0 is never used */
static uint32_t next_obj = 1;
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
    po->objs = 2;
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

static void fdobj_maybe_free(struct fd_obj *o);

static void wait_on(struct thread *t, struct fd_obj *o, struct list_node *queue)
{
    t->io_obj = o;
    o->waiting++;
    list_push_back(queue, &t->io_link);
}

/* Ends t's wait (t is already off the wait list). */
static void wait_done(struct thread *t)
{
    struct fd_obj *o = t->io_obj;
    t->io_obj = NULL;
    o->waiting--;
    fdobj_maybe_free(o);
}

static void reply_io(struct thread *t, int64_t ret)
{
    struct reply_obj *rep = t->park_reply;
    t->park_reply = NULL;
    t->park_kind = PARK_NONE;
    wait_done(t);
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

/* ---- objects referenced by vfsd ---- */

static int obj_register(struct fd_obj *o)
{
    for (uint32_t i = 0; i < FASTIX_PROCD_MAX_FDOBJS - 1u; i++) {
        uint32_t idx = 1u + (next_obj - 1u + i) % (FASTIX_PROCD_MAX_FDOBJS - 1u);
        if (objs[idx] == NULL) {
            objs[idx] = o;
            o->index = idx;
            next_obj = idx + 1u;
            return 0;
        }
    }
    return -FASTIX_ENFILE;
}

static void fdobj_maybe_free(struct fd_obj *o)
{
    if (!o->released || o->waiting != 0) {
        return;
    }
    if (o->kind == FDOBJ_EVENTFD) {
        slab_free(&efd_cache, o->u.efd);
    } else if (--o->u.pipe->objs == 0) {
        pipe_obj_destroy(o->u.pipe);
    }
    slab_free(&fdobj_cache, o);
}

void pipe_obj_release(uint32_t index)
{
    struct fd_obj *o = index < FASTIX_PROCD_MAX_FDOBJS ? objs[index] : NULL;
    if (o == NULL) {
        return;
    }
    objs[index] = NULL;
    o->index = 0;
    o->released = true;
    if (o->kind == FDOBJ_PIPE_RD && --o->u.pipe->readers == 0) {
        bool notify = false;
        ring_close_read(o->u.pipe->ring, &notify);
        pipe_wake_writers(o->u.pipe); /* blocked writers get EPIPE */
    } else if (o->kind == FDOBJ_PIPE_WR && --o->u.pipe->writers == 0) {
        bool notify = false;
        ring_close_write(o->u.pipe->ring, &notify);
        pipe_wake_readers(o->u.pipe); /* blocked readers get EOF */
    }
    fdobj_maybe_free(o);
}

void pipe_obj_setfl(uint32_t index, uint32_t flags)
{
    struct fd_obj *o = index < FASTIX_PROCD_MAX_FDOBJS ? objs[index] : NULL;
    if (o != NULL) {
        o->status_flags = (o->status_flags & ~FASTIX_O_NONBLOCK) | (flags & FASTIX_O_NONBLOCK);
    }
}

uint64_t pipe_obj_count(void)
{
    return fdobj_cache.in_use;
}

void pipe_cancel_wait(struct thread *t)
{
    if (t->park_kind == PARK_PIPE_READ || t->park_kind == PARK_PIPE_WRITE ||
        t->park_kind == PARK_EFD_READ || t->park_kind == PARK_EFD_WRITE) {
        list_remove(&t->io_link);
        wait_done(t);
    }
}

/* Object behind fd for an I/O call, or NULL with *err set (-EBADF, or -ESTALEFD + *owner). */
static struct fd_obj *obj_from_fd(const struct proc *p, int32_t fd, int64_t *err,
                                  uint32_t *owner)
{
    uint32_t word = vfslink_fd_word(p, fd);
    *owner = word >> FASTIX_FDEXP_OWNER_SHIFT;
    if (word == 0) {
        *err = -FASTIX_EBADF;
        return NULL;
    }
    struct fd_obj *o = NULL;
    if (*owner == FASTIX_OWNER_PROCD) {
        uint32_t idx = word & FASTIX_FDEXP_INDEX_MASK;
        o = idx < FASTIX_PROCD_MAX_FDOBJS ? objs[idx] : NULL;
    }
    *err = o == NULL ? -FASTIX_ESTALEFD : 0;
    return o;
}

/* ---- syscalls ---- */

static struct fd_obj *fdobj_new(enum fdobj_kind kind, uint32_t status_flags)
{
    struct fd_obj *o = slab_alloc(&fdobj_cache);
    if (o == NULL) {
        return NULL;
    }
    *o = (struct fd_obj){ .kind = kind, .status_flags = status_flags };
    if (obj_register(o) != 0) {
        slab_free(&fdobj_cache, o);
        return NULL;
    }
    return o;
}

/* Frees an object that was never handed to vfsd (NULL is ignored). */
static void fdobj_discard(struct fd_obj *o)
{
    if (o != NULL) {
        objs[o->index] = NULL;
        slab_free(&fdobj_cache, o);
    }
}

/* Install flags for vfsd: access mode | O_NONBLOCK | O_CLOEXEC | object kind. */
static uint32_t install_flags(uint32_t acc, uint32_t flags, uint32_t kind)
{
    return acc | (flags & (FASTIX_O_NONBLOCK | FASTIX_O_CLOEXEC)) |
           (kind << FASTIX_VFS_KIND_SHIFT);
}

int pipe_sys_pipe2(struct thread *t, uint64_t fds_u, uint32_t flags)
{
    if ((flags & ~(FASTIX_O_NONBLOCK | FASTIX_O_CLOEXEC)) != 0) {
        return -FASTIX_EINVAL;
    }
    int32_t probe[2] = { -1, -1 }; /* fail with EFAULT before anything is created */
    int err = vms_write_user(t->proc->vs, fds_u, probe, sizeof(probe));
    if (err != 0) {
        return err;
    }
    struct pipe_obj *po;
    if ((err = pipe_obj_create(&po)) != 0) {
        return err;
    }
    uint32_t sflags = flags & FASTIX_O_NONBLOCK;
    struct fd_obj *rd = fdobj_new(FDOBJ_PIPE_RD, sflags);
    struct fd_obj *wr = fdobj_new(FDOBJ_PIPE_WR, sflags);
    if (rd == NULL || wr == NULL) {
        fdobj_discard(rd);
        fdobj_discard(wr);
        pipe_obj_destroy(po);
        return -FASTIX_ENOMEM;
    }
    rd->u.pipe = po;
    wr->u.pipe = po;
    uint32_t obj[2] = { rd->index, wr->index };
    uint32_t iflags[2] = { install_flags(FASTIX_O_RDONLY, flags, FASTIX_OBJ_PIPE),
                           install_flags(FASTIX_O_WRONLY, flags, FASTIX_OBJ_PIPE) };
    err = vfslink_install(t, 2, obj, iflags, fds_u);
    if (err != 0) {
        pipe_obj_release(obj[0]); /* frees the pipe with its second end */
        pipe_obj_release(obj[1]);
    }
    return err;
}

int pipe_sys_eventfd2(struct thread *t, uint32_t initval, uint32_t flags)
{
    uint32_t allowed = FASTIX_EFD_SEMAPHORE | FASTIX_EFD_NONBLOCK | FASTIX_EFD_CLOEXEC;
    if ((flags & ~allowed) != 0) {
        return -FASTIX_EINVAL;
    }
    struct eventfd_obj *efd = slab_alloc(&efd_cache);
    if (efd == NULL) {
        return -FASTIX_ENOMEM;
    }
    efd->counter = initval;
    list_init(&efd->rd_waiters);
    list_init(&efd->wr_waiters);
    struct fd_obj *o = fdobj_new(FDOBJ_EVENTFD, flags & (FASTIX_EFD_NONBLOCK | FASTIX_EFD_SEMAPHORE));
    if (o == NULL) {
        slab_free(&efd_cache, efd);
        return -FASTIX_ENOMEM;
    }
    o->u.efd = efd;
    uint32_t obj[2] = { o->index, 0 };
    uint32_t iflags[2] = { install_flags(FASTIX_O_RDWR, flags, FASTIX_OBJ_EVENTFD), 0 };
    int err = vfslink_install(t, 1, obj, iflags, 0);
    if (err != 0) {
        pipe_obj_release(o->index);
    }
    return err;
}

static int64_t pipe_read(struct thread *t, struct fd_obj *o, uint64_t buf_u, uint64_t len,
                         enum park_kind *park_out)
{
    if (len == 0) {
        return 0;
    }
    struct pipe_obj *po = o->u.pipe;
    size_t chunk = len < IO_CHUNK_MAX ? (size_t)len : IO_CHUNK_MAX;
    bool need_notify = false;
    int64_t got = ring_read(po->ring, io_buf, chunk, &need_notify);
    if (got == -FASTIX_EAGAIN) {
        if (o->status_flags & FASTIX_O_NONBLOCK) {
            return -FASTIX_EAGAIN;
        }
        t->io_buf_u = buf_u;
        t->io_len = len;
        t->io_done = 0;
        wait_on(t, o, &po->rd_waiters);
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

static int64_t efd_read(struct thread *t, struct fd_obj *o, uint64_t buf_u, uint64_t len,
                        enum park_kind *park_out)
{
    if (len < sizeof(uint64_t)) {
        return -FASTIX_EINVAL;
    }
    struct eventfd_obj *efd = o->u.efd;
    if (efd->counter == 0) {
        if (o->status_flags & FASTIX_O_NONBLOCK) {
            return -FASTIX_EAGAIN;
        }
        t->io_buf_u = buf_u;
        t->io_len = len;
        t->io_done = 0;
        wait_on(t, o, &efd->rd_waiters);
        *park_out = PARK_EFD_READ;
        return 0;
    }
    uint64_t val = (o->status_flags & FASTIX_EFD_SEMAPHORE) ? 1ULL : efd->counter;
    efd->counter -= val;
    int err = vms_write_user(t->proc->vs, buf_u, &val, sizeof(val));
    efd_wake_writers(efd);
    return err != 0 ? err : (int64_t)sizeof(val);
}

int64_t pipe_sys_read(struct thread *t, int32_t fd, uint64_t buf_u, uint64_t len,
                      enum park_kind *park_out, uint32_t *owner)
{
    *park_out = PARK_NONE;
    int64_t err;
    struct fd_obj *o = obj_from_fd(t->proc, fd, &err, owner);
    if (o == NULL) {
        return err;
    }
    if (o->kind == FDOBJ_PIPE_RD) {
        return pipe_read(t, o, buf_u, len, park_out);
    }
    if (o->kind == FDOBJ_EVENTFD) {
        return efd_read(t, o, buf_u, len, park_out);
    }
    return -FASTIX_EBADF; /* write end */
}

static int64_t pipe_write(struct thread *t, struct fd_obj *o, uint64_t buf_u, uint64_t len,
                          enum park_kind *park_out)
{
    struct pipe_obj *po = o->u.pipe;
    bool nonblock = (o->status_flags & FASTIX_O_NONBLOCK) != 0;
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
    t->io_buf_u = buf_u;
    t->io_len = len;
    t->io_done = done;
    wait_on(t, o, &po->wr_waiters);
    *park_out = PARK_PIPE_WRITE;
    return 0;
}

static int64_t efd_write(struct thread *t, struct fd_obj *o, uint64_t buf_u, uint64_t len,
                         enum park_kind *park_out)
{
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
        if (o->status_flags & FASTIX_O_NONBLOCK) {
            return -FASTIX_EAGAIN;
        }
        t->io_efd_val = val;
        t->io_done = 0;
        wait_on(t, o, &efd->wr_waiters);
        *park_out = PARK_EFD_WRITE;
        return 0;
    }
    efd->counter += val;
    efd_wake_readers(efd);
    return (int64_t)sizeof(val);
}

int64_t pipe_sys_write(struct thread *t, int32_t fd, uint64_t buf_u, uint64_t len,
                       enum park_kind *park_out, uint32_t *owner)
{
    *park_out = PARK_NONE;
    int64_t err;
    struct fd_obj *o = obj_from_fd(t->proc, fd, &err, owner);
    if (o == NULL) {
        return err;
    }
    if (o->kind == FDOBJ_PIPE_WR) {
        return pipe_write(t, o, buf_u, len, park_out);
    }
    if (o->kind == FDOBJ_EVENTFD) {
        return efd_write(t, o, buf_u, len, park_out);
    }
    return -FASTIX_EBADF; /* read end */
}
