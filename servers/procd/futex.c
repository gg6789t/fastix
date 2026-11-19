/* futex.c - futex waitqueues and robust list exit handling (see futex.h). */
#include <stdbool.h>
#include <stdint.h>

#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <fastix/futex_abi.h>
#include <fastix/time_abi.h>
#include <libos/list.h>

#include "futex.h"
#include "timer.h"
#include "vdso.h"

#define FUTEX_BUCKETS 64u

static struct list_node buckets[FUTEX_BUCKETS];

void futex_module_init(void)
{
    for (unsigned i = 0; i < FUTEX_BUCKETS; i++) {
        list_init(&buckets[i]);
    }
}

static unsigned key_bucket(struct futex_key k)
{
    uint64_t h = (k.a ^ (k.a >> 12)) * 0x9E3779B97F4A7C15ULL ^ (k.b * 0xBF58476D1CE4E5B9ULL);
    return (unsigned)((h ^ (h >> 32)) % FUTEX_BUCKETS);
}

static bool key_eq(struct futex_key x, struct futex_key y)
{
    return x.a == y.a && x.b == y.b;
}

/* Resolves (vs, uaddr, priv) to a private (vs, uaddr) or shared (frame, page_off) key. */
static int make_key(struct vmspace *vs, uint64_t uaddr, bool priv, struct futex_key *out)
{
    if ((uaddr & 3u) != 0) {
        return -FASTIX_EINVAL;
    }
    if (priv) {
        out->a = (uint64_t)(uintptr_t)vs;
        out->b = uaddr;
        return 0;
    }
    struct frame *f = NULL;
    bool is_shared = false;
    int err = vms_get_frame(vs, uaddr, false, &f, &is_shared);
    if (err != 0) {
        return err;
    }
    if (is_shared) {
        out->a = (uint64_t)(uintptr_t)f;
        out->b = uaddr & (FASTIX_PAGE_SIZE - 1u);
    } else {
        out->a = (uint64_t)(uintptr_t)vs;
        out->b = uaddr;
    }
    return 0;
}

static uint64_t futex_now_ns(int clockid)
{
    uint64_t mono = vdso_mono_ns();
    return clockid == FASTIX_CLOCK_REALTIME ? (mono + vdso_realtime_offset_ns()) : mono;
}

void futex_cancel_wait(struct thread *t)
{
    if (t->park_kind != PARK_FUTEX) {
        return;
    }
    list_remove(&t->futex_link);
    if (t->futex_deadline_ns != 0) {
        t->futex_deadline_ns = 0;
        timer_external_dec();
    }
}

void futex_tick(void)
{
    for (unsigned b = 0; b < FUTEX_BUCKETS; b++) {
        struct list_node *n, *tmp;
        LIST_FOREACH_SAFE(n, tmp, &buckets[b]) {
            struct thread *t = LIST_ENTRY(n, struct thread, futex_link);
            if (t->futex_deadline_ns == 0) {
                continue;
            }
            if (futex_now_ns(t->futex_clock) < t->futex_deadline_ns) {
                continue;
            }
            list_remove(n);
            t->futex_deadline_ns = 0;
            timer_external_dec();
            struct reply_obj *rep = t->park_reply;
            t->park_reply = NULL;
            t->park_kind = PARK_NONE;
            uint64_t mr[1] = { (uint64_t)-FASTIX_ETIMEDOUT };
            reply_send(rep, 1, mr);
        }
    }
}

static int wake_by_key(struct futex_key key, uint32_t nr, uint32_t bitset)
{
    if (nr == 0) {
        return 0;
    }
    struct list_node *head = &buckets[key_bucket(key)];
    struct list_node *n, *tmp;
    int woken = 0;
    LIST_FOREACH_SAFE(n, tmp, head) {
        if ((uint32_t)woken >= nr) {
            break;
        }
        struct thread *t = LIST_ENTRY(n, struct thread, futex_link);
        if (!key_eq(t->futex_key, key) || (t->futex_bitset & bitset) == 0) {
            continue;
        }
        list_remove(n);
        if (t->futex_deadline_ns != 0) {
            t->futex_deadline_ns = 0;
            timer_external_dec();
        }
        struct reply_obj *rep = t->park_reply;
        t->park_reply = NULL;
        t->park_kind = PARK_NONE;
        uint64_t mr[1] = { 0 };
        reply_send(rep, 1, mr);
        woken++;
    }
    return woken;
}

int futex_wake_internal(struct vmspace *vs, uint64_t uaddr, uint32_t nr, bool priv, uint32_t bitset)
{
    struct futex_key key;
    int err = make_key(vs, uaddr, priv, &key);
    if (err != 0) {
        return err;
    }
    return wake_by_key(key, nr, bitset);
}

static int do_wait(struct thread *t, uint64_t uaddr, uint32_t val, uint64_t timeout_u,
                   bool priv, bool abstime, int clockid, uint32_t bitset, bool *park)
{
    if (bitset == 0) {
        return -FASTIX_EINVAL;
    }
    struct futex_key key;
    int err = make_key(t->proc->vs, uaddr, priv, &key);
    if (err != 0) {
        return err;
    }
    uint32_t cur = 0;
    err = vms_read(t->proc->vs, uaddr, &cur, sizeof(cur));
    if (err != 0) {
        return err;
    }
    if (cur != val) {
        return -FASTIX_EAGAIN;
    }

    uint64_t deadline = 0;
    if (timeout_u != 0) {
        struct fastix_timespec ts;
        err = vms_read(t->proc->vs, timeout_u, &ts, sizeof(ts));
        if (err != 0) {
            return err;
        }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || (uint64_t)ts.tv_nsec >= FASTIX_NSEC_PER_SEC) {
            return -FASTIX_EINVAL;
        }
        uint64_t now = futex_now_ns(clockid);
        uint64_t ns = (uint64_t)ts.tv_sec * FASTIX_NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
        deadline = abstime ? ns : (now + ns);
        if (deadline <= now) {
            return -FASTIX_ETIMEDOUT;
        }
    }

    t->futex_key = key;
    t->futex_bitset = bitset;
    t->futex_clock = clockid;
    t->futex_deadline_ns = deadline;
    list_push_back(&buckets[key_bucket(key)], &t->futex_link);
    if (deadline != 0) {
        timer_external_inc();
    }
    *park = true;
    return 0;
}

static int do_requeue(struct thread *t, uint64_t uaddr, uint64_t uaddr2, uint32_t nr_wake,
                      uint32_t nr_requeue, bool priv, bool cmp, uint32_t cmp_val)
{
    struct futex_key key1, key2;
    int err = make_key(t->proc->vs, uaddr, priv, &key1);
    if (err == 0) {
        err = make_key(t->proc->vs, uaddr2, priv, &key2);
    }
    if (err != 0) {
        return err;
    }
    if (cmp) {
        uint32_t cur = 0;
        err = vms_read(t->proc->vs, uaddr, &cur, sizeof(cur));
        if (err != 0) {
            return err;
        }
        if (cur != cmp_val) {
            return -FASTIX_EAGAIN;
        }
    }
    int woken = wake_by_key(key1, nr_wake, FASTIX_FUTEX_BITSET_MATCH_ANY);
    int moved = 0;
    if (nr_requeue > 0 && !key_eq(key1, key2)) {
        struct list_node *src = &buckets[key_bucket(key1)];
        struct list_node *dst = &buckets[key_bucket(key2)];
        struct list_node *n, *tmp;
        LIST_FOREACH_SAFE(n, tmp, src) {
            if ((uint32_t)moved >= nr_requeue) {
                break;
            }
            struct thread *w = LIST_ENTRY(n, struct thread, futex_link);
            if (!key_eq(w->futex_key, key1)) {
                continue;
            }
            list_remove(n);
            w->futex_key = key2;
            list_push_back(dst, n);
            moved++;
        }
    }
    return cmp ? (woken + moved) : woken;
}

int futex_sys_op(struct thread *t, uint64_t uaddr, uint32_t op, uint32_t val,
                 uint64_t val2, uint64_t uaddr2, uint32_t val3, bool *park)
{
    uint32_t cmd = op & FASTIX_FUTEX_CMD_MASK;
    bool priv = (op & FASTIX_FUTEX_PRIVATE_FLAG) != 0;
    int clockid = (op & FASTIX_FUTEX_CLOCK_REALTIME) ? FASTIX_CLOCK_REALTIME
                                                     : FASTIX_CLOCK_MONOTONIC;
    *park = false;

    switch (cmd) {
    case FASTIX_FUTEX_WAIT:
        return do_wait(t, uaddr, val, val2, priv, false, FASTIX_CLOCK_MONOTONIC,
                       FASTIX_FUTEX_BITSET_MATCH_ANY, park);
    case FASTIX_FUTEX_WAIT_BITSET:
        return do_wait(t, uaddr, val, val2, priv, true, clockid, val3, park);
    case FASTIX_FUTEX_WAKE:
        return futex_wake_internal(t->proc->vs, uaddr, val, priv, FASTIX_FUTEX_BITSET_MATCH_ANY);
    case FASTIX_FUTEX_WAKE_BITSET:
        if (val3 == 0) {
            return -FASTIX_EINVAL;
        }
        return futex_wake_internal(t->proc->vs, uaddr, val, priv, val3);
    case FASTIX_FUTEX_REQUEUE:
        return do_requeue(t, uaddr, uaddr2, val, (uint32_t)val2, priv, false, 0);
    case FASTIX_FUTEX_CMP_REQUEUE:
        return do_requeue(t, uaddr, uaddr2, val, (uint32_t)val2, priv, true, val3);
    default:
        return -FASTIX_ENOSYS;
    }
}

/* ---- robust lists ---- */

int futex_sys_set_robust_list(struct thread *t, uint64_t head_u, uint64_t len)
{
    if (len != sizeof(struct fastix_robust_list_head)) {
        return -FASTIX_EINVAL;
    }
    t->robust_list = head_u;
    return 0;
}

int futex_sys_get_robust_list(struct thread *caller, uint32_t pid, uint64_t head_ptr_u,
                              uint64_t len_ptr_u)
{
    struct thread *target = pid == 0 ? caller : proc_find_thread_global(pid);
    if (target == NULL) {
        return -FASTIX_ESRCH;
    }
    uint64_t len = sizeof(struct fastix_robust_list_head);
    int err = vms_write_user(caller->proc->vs, head_ptr_u, &target->robust_list, sizeof(uint64_t));
    if (err == 0) {
        err = vms_write_user(caller->proc->vs, len_ptr_u, &len, sizeof(len));
    }
    return err;
}

static void handle_robust_entry(struct thread *t, uint64_t entry_u, int64_t futex_offset)
{
    /* Low bit of entry pointer marks a PI futex in Linux ABI; mask it off. */
    uint64_t addr = (entry_u & ~1ULL) + (uint64_t)futex_offset;
    uint32_t word = 0;
    if (vms_read(t->proc->vs, addr, &word, sizeof(word)) != 0) {
        return;
    }
    if ((word & FASTIX_FUTEX_TID_MASK) != t->tid) {
        return;
    }
    uint32_t died = (word & FASTIX_FUTEX_WAITERS) | FASTIX_FUTEX_OWNER_DIED;
    if (vms_write_user(t->proc->vs, addr, &died, sizeof(died)) != 0) {
        return;
    }
    if (word & FASTIX_FUTEX_WAITERS) {
        /* Wake both private and shared keys so whichever mode waiters used wakes up. */
        (void)futex_wake_internal(t->proc->vs, addr, 1, true, FASTIX_FUTEX_BITSET_MATCH_ANY);
        (void)futex_wake_internal(t->proc->vs, addr, 1, false, FASTIX_FUTEX_BITSET_MATCH_ANY);
    }
}

void futex_exit_robust_list(struct thread *t)
{
    uint64_t head_u = t->robust_list;
    if (head_u == 0 || t->proc->vs == NULL) {
        return;
    }
    t->robust_list = 0;

    struct fastix_robust_list_head head;
    if (vms_read(t->proc->vs, head_u, &head, sizeof(head)) != 0) {
        return;
    }
    uint64_t pending = head.list_op_pending;
    uint64_t cur = head.list.next;
    for (unsigned count = 0; count < FASTIX_ROBUST_LIST_LIMIT && cur != 0 && cur != head_u;
         count++) {
        uint64_t next = 0;
        if (vms_read(t->proc->vs, cur & ~1ULL, &next, sizeof(next)) != 0) {
            break;
        }
        if (cur != pending) {
            handle_robust_entry(t, cur, head.futex_offset);
        }
        cur = next;
    }
    if (pending != 0) {
        handle_robust_entry(t, pending, head.futex_offset);
    }
}
