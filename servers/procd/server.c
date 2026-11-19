/* server.c - procd main loop and syscall dispatch (see server.h). */
#include <stdbool.h>

#include <arch/counter.h>
#include <arch/fault.h>
#include <arch/qemu_exit.h>
#include <fastix/abi.h>
#include <fastix/cspace.h>
#include <fastix/errno.h>
#include <fastix/signal_abi.h>
#include <libos/kprint.h>

#include "exec.h"
#include "futex.h"
#include "images.h"
#include "pipe.h"
#include "proc.h"
#include "reply.h"
#include "server.h"
#include "signal.h"
#include "timer.h"
#include "vdso.h"

#define CALL_MAX_ARGS 6
#define CALL_MAX_RET  7

enum outcome {
    OUT_REPLY,  /* answer now with ret[0..nret-1] */
    OUT_PARKED, /* the caller waits; its Reply object was parked */
    OUT_NONE,   /* no answer: the caller exited or was restarted via WriteRegisters */
};

struct call {
    struct thread *t;
    struct proc *p;
    seL4_Word label;
    uint64_t arg[CALL_MAX_ARGS];
    uint64_t ret[CALL_MAX_RET];
    unsigned nret;
};

static struct kobj endpoint;
static struct reply_obj *current; /* Reply object used for the next receive */

/* The most recent process killed by a fault (FASTIX_TEST_LAST_KILL). */
static struct {
    uint32_t pid;
    int sig, code;
    uint64_t addr;
} last_kill;

int server_init(void)
{
    int err = kobj_alloc(seL4_EndpointObject, 0, &endpoint);
    return err != 0 ? err : reply_get(&current);
}

seL4_CPtr server_endpoint(void)
{
    return endpoint.cap;
}

/* Takes the current Reply object for parking and installs a fresh one for the loop. */
static int take_reply(struct reply_obj **out)
{
    struct reply_obj *fresh;
    int err = reply_get(&fresh);
    if (err != 0) {
        return err;
    }
    *out = current;
    current = fresh;
    return 0;
}

static enum outcome sys_wait4(struct call *c)
{
    int32_t wpid = (int32_t)c->arg[0];
    uint64_t status_ptr = c->arg[1];
    uint64_t options = c->arg[2];
    uint32_t child = 0;
    int status = 0;

    int r = proc_wait(c->p, wpid, options, &child, &status);
    if (r == 1) {
        struct reply_obj *rep;
        int err = take_reply(&rep);
        if (err == 0) {
            c->t->park_reply = rep;
            c->t->park_kind = PARK_WAIT4;
            c->t->wait_pid = wpid;
            c->t->wait_options = options;
            c->t->wait_status_ptr = status_ptr;
            return OUT_PARKED;
        }
        r = err;
    }
    if (r == 0 && child != 0 && status_ptr != 0) {
        int32_t st32 = (int32_t)status;
        int werr = vms_write_user(c->p->vs, status_ptr, &st32, sizeof(st32));
        if (werr != 0) {
            r = werr;
        }
    }
    c->ret[0] = r < 0 ? (uint64_t)(int64_t)r : child;
    c->ret[1] = (uint64_t)(int64_t)status;
    c->nret = 2;
    return OUT_REPLY;
}

static enum outcome sys_clock_gettime(struct call *c)
{
    switch (c->arg[0]) {
    case FASTIX_CLOCK_MONOTONIC:
    case FASTIX_CLOCK_BOOTTIME:
        c->ret[1] = vdso_mono_ns();
        break;
    case FASTIX_CLOCK_REALTIME:
        c->ret[1] = vdso_mono_ns() + vdso_realtime_offset_ns();
        break;
    default:
        c->ret[0] = (uint64_t)-FASTIX_EINVAL;
        return OUT_REPLY;
    }
    c->nret = 2;
    return OUT_REPLY;
}

static enum outcome sys_fork(struct call *c)
{
    struct proc *child;
    int err = proc_fork(c->t, &child);
    c->ret[0] = err != 0 ? (uint64_t)(int64_t)err : child->pid;
    return OUT_REPLY;
}

static enum outcome sys_vfork(struct call *c)
{
    /* If called from a worker thread or multithreaded process, fall back to COW fork so
     * sibling threads cannot race on the shared address space or main IPC buffer. */
    if (c->t->tid_slot != 0 || c->p->live_threads > 1) {
        return sys_fork(c);
    }
    struct reply_obj *rep;
    int err = take_reply(&rep);
    if (err != 0) {
        c->ret[0] = (uint64_t)(int64_t)err;
        return OUT_REPLY;
    }
    struct proc *child;
    err = proc_vfork(c->t, rep, &child);
    if (err != 0) {
        /* Restore the reply object we took so the loop replies to the caller with -errno. */
        reply_put(current);
        current = rep;
        c->ret[0] = (uint64_t)(int64_t)err;
        return OUT_REPLY;
    }
    return OUT_PARKED;
}

static enum outcome sys_execve(struct call *c)
{
    int err = proc_execve(c->t, c->arg[0], c->arg[1], c->arg[2]);
    if (err == 0) {
        return OUT_NONE; /* caller is already running the new image */
    }
    c->ret[0] = (uint64_t)(int64_t)err;
    return OUT_REPLY;
}

static enum outcome sys_posix_spawn(struct call *c)
{
    uint32_t child_pid = 0;
    int err = proc_posix_spawn(c->t, c->arg[0], &child_pid);
    c->ret[0] = err != 0 ? (uint64_t)(int64_t)err : child_pid;
    return OUT_REPLY;
}

static enum outcome sys_sigsuspend(struct call *c)
{
    int r = sig_sys_suspend(c->t, c->arg[0]);
    if (r == 1) {
        struct reply_obj *rep;
        int err = take_reply(&rep);
        if (err == 0) {
            c->t->park_reply = rep;
            c->t->park_kind = PARK_SIGSUSPEND;
            return OUT_PARKED;
        }
        r = err;
    }
    c->ret[0] = (uint64_t)(int64_t)r;
    return OUT_REPLY;
}

static enum outcome sys_nanosleep(struct call *c, int clockid, uint64_t flags, uint64_t req_u,
                                  uint64_t rem_u)
{
    int r = timer_sys_nanosleep(c->t, clockid, flags, req_u, rem_u);
    if (r == 1) {
        struct reply_obj *rep;
        int err = take_reply(&rep);
        if (err == 0) {
            c->t->park_reply = rep;
            c->t->park_kind = PARK_SLEEP;
            return OUT_PARKED;
        }
        timer_cancel_sleep(c->t);
        r = err;
    }
    c->ret[0] = (uint64_t)(int64_t)r;
    return OUT_REPLY;
}

static enum outcome sys_io_rw(struct call *c, bool is_write)
{
    enum park_kind pk = PARK_NONE;
    int64_t r = is_write ? pipe_sys_write(c->t, (int32_t)c->arg[0], c->arg[1], c->arg[2], &pk)
                         : pipe_sys_read(c->t, (int32_t)c->arg[0], c->arg[1], c->arg[2], &pk);
    if (pk != PARK_NONE) {
        struct reply_obj *rep;
        int err = take_reply(&rep);
        if (err == 0) {
            c->t->park_reply = rep;
            c->t->park_kind = pk;
            return OUT_PARKED;
        }
        pipe_cancel_wait(c->t);
        r = err;
    }
    c->ret[0] = (uint64_t)r;
    return OUT_REPLY;
}

static enum outcome sys_futex(struct call *c)
{
    bool park = false;
    int r = futex_sys_op(c->t, c->arg[0], (uint32_t)c->arg[1], (uint32_t)c->arg[2], c->arg[3],
                         c->arg[4], (uint32_t)c->arg[5], &park);
    if (park) {
        struct reply_obj *rep;
        int err = take_reply(&rep);
        if (err == 0) {
            c->t->park_reply = rep;
            c->t->park_kind = PARK_FUTEX;
            return OUT_PARKED;
        }
        futex_cancel_wait(c->t);
        r = err;
    }
    c->ret[0] = (uint64_t)(int64_t)r;
    return OUT_REPLY;
}

static enum outcome sys_getres3(struct call *c, uint32_t r, uint32_t e, uint32_t s)
{
    uint32_t vals[3] = { r, e, s };
    for (int i = 0; i < 3; i++) {
        if (c->arg[i] != 0) {
            int err = vms_write_user(c->p->vs, c->arg[i], &vals[i], sizeof(uint32_t));
            if (err != 0) {
                c->ret[0] = (uint64_t)(int64_t)err;
                return OUT_REPLY;
            }
        }
    }
    c->ret[0] = 0;
    c->ret[1] = r;
    c->ret[2] = e;
    c->ret[3] = s;
    c->nret = 4;
    return OUT_REPLY;
}

#ifdef FASTIX_TEST_HOOKS
static enum outcome test_hook(struct call *c)
{
    struct kobj_stats ks = kobj_stats();
    struct vm_stats vm = vms_stats();

    switch (c->label) {
    case FASTIX_TEST_PING:
        c->ret[0] = c->arg[0] + 1;
        return OUT_REPLY;
    case FASTIX_TEST_STATS:
        c->ret[1] = ks.free_bytes;
        c->ret[2] = ks.slots_used;
        c->ret[3] = frame_live();
        c->ret[4] = vm.pages;
        c->ret[5] = vm.vmas + (vm.pts << 32); /* low: VMAs, high: page tables */
        c->ret[6] = proc_count();
        c->nret = 7;
        return OUT_REPLY;
    case FASTIX_TEST_VM_STATS:
        c->ret[1] = c->p->vs->vmas.count;
        c->ret[2] = c->p->vs->pages.count;
        c->nret = 3;
        return OUT_REPLY;
    case FASTIX_TEST_LAST_KILL:
        if (last_kill.pid != c->arg[0]) {
            c->ret[0] = (uint64_t)-FASTIX_ESRCH;
            return OUT_REPLY;
        }
        c->ret[1] = (uint64_t)last_kill.sig;
        c->ret[2] = (uint64_t)last_kill.code;
        c->ret[3] = last_kill.addr;
        c->nret = 4;
        return OUT_REPLY;
    case FASTIX_TEST_IPC_COUNT:
        c->ret[1] = c->p->ipc_count;
        c->nret = 2;
        return OUT_REPLY;
    case FASTIX_TEST_SPAWN: {
        const struct image *img = image_get(c->arg[0]);
        if (img == NULL) {
            c->ret[0] = (uint64_t)-FASTIX_ENOENT;
            return OUT_REPLY;
        }
        const char *argv[] = { img->name, NULL };
        const char *envp[] = { "FASTIX=1", NULL };
        struct load_args la = {
            .elf = img->start,
            .elf_size = (size_t)(img->end - img->start),
            .argv = argv,
            .envp = envp,
            .random_seed = arch_read_counter() ^ c->p->pid,
        };
        struct proc *child;
        int err = proc_spawn(&la, c->p->pid, &child);
        c->ret[0] = err != 0 ? (uint64_t)(int64_t)err : child->pid;
        return OUT_REPLY;
    }
    default:
        c->ret[0] = (uint64_t)-FASTIX_ENOSYS;
        return OUT_REPLY;
    }
}
#endif

static enum outcome handle_syscall(struct call *c)
{
    struct proc *p = c->p;
    uint64_t addr;
    uint32_t val;
    int err;

    switch (c->label) {
    case FASTIX_SYS_exit:
        proc_thread_exit(c->t, (int)c->arg[0]);
        return OUT_NONE;
    case FASTIX_SYS_exit_group:
        proc_exit(p, FASTIX_W_EXITED((int)c->arg[0]));
        return OUT_NONE;
    case FASTIX_SYS_thread_create: {
        uint32_t new_tid = 0;
        err = proc_thread_create(c->t, c->arg[0], c->arg[1], c->arg[2], c->arg[3], c->arg[4],
                                 (uint32_t)c->arg[5], &new_tid);
        c->ret[0] = err != 0 ? (uint64_t)(int64_t)err : new_tid;
        return OUT_REPLY;
    }
    case FASTIX_SYS_set_tid_address:
        c->t->clear_child_tid = c->arg[0];
        c->ret[0] = c->t->tid;
        return OUT_REPLY;
    case FASTIX_SYS_set_tls:
        c->ret[0] = (uint64_t)(int64_t)kobj_sel4_err(seL4_TCB_SetTLSBase(c->t->tcb.cap, c->arg[0]));
        return OUT_REPLY;
    case FASTIX_SYS_fork:
        return sys_fork(c);
    case FASTIX_SYS_vfork:
        return sys_vfork(c);
    case FASTIX_SYS_posix_spawn_fast:
        return sys_posix_spawn(c);
    case FASTIX_SYS_execve:
        return sys_execve(c);
    case FASTIX_SYS_wait4:
        return sys_wait4(c);
    case FASTIX_SYS_getpid:
        c->ret[0] = p->pid;
        return OUT_REPLY;
    case FASTIX_SYS_getppid:
        c->ret[0] = p->ppid;
        return OUT_REPLY;
    case FASTIX_SYS_gettid:
        c->ret[0] = c->t->tid;
        return OUT_REPLY;
    case FASTIX_SYS_getpgid:
        err = proc_getpgid(p, (uint32_t)c->arg[0], &val);
        c->ret[0] = err != 0 ? (uint64_t)(int64_t)err : val;
        return OUT_REPLY;
    case FASTIX_SYS_setpgid:
        c->ret[0] = (uint64_t)(int64_t)proc_setpgid(p, (uint32_t)c->arg[0], (uint32_t)c->arg[1]);
        return OUT_REPLY;
    case FASTIX_SYS_getsid:
        err = proc_getsid(p, (uint32_t)c->arg[0], &val);
        c->ret[0] = err != 0 ? (uint64_t)(int64_t)err : val;
        return OUT_REPLY;
    case FASTIX_SYS_setsid:
        err = proc_setsid(p, &val);
        c->ret[0] = err != 0 ? (uint64_t)(int64_t)err : val;
        return OUT_REPLY;
    case FASTIX_SYS_getuid:
        c->ret[0] = p->uid;
        return OUT_REPLY;
    case FASTIX_SYS_geteuid:
        c->ret[0] = p->euid;
        return OUT_REPLY;
    case FASTIX_SYS_getgid:
        c->ret[0] = p->gid;
        return OUT_REPLY;
    case FASTIX_SYS_getegid:
        c->ret[0] = p->egid;
        return OUT_REPLY;
    case FASTIX_SYS_setuid:
        c->ret[0] = (uint64_t)(int64_t)proc_setuid(p, (uint32_t)c->arg[0]);
        return OUT_REPLY;
    case FASTIX_SYS_seteuid:
        c->ret[0] = (uint64_t)(int64_t)proc_seteuid(p, (uint32_t)c->arg[0]);
        return OUT_REPLY;
    case FASTIX_SYS_setreuid:
        c->ret[0] = (uint64_t)(int64_t)proc_setreuid(p, (uint32_t)c->arg[0], (uint32_t)c->arg[1]);
        return OUT_REPLY;
    case FASTIX_SYS_setresuid:
        c->ret[0] = (uint64_t)(int64_t)proc_setresuid(p, (uint32_t)c->arg[0], (uint32_t)c->arg[1],
                                                      (uint32_t)c->arg[2]);
        return OUT_REPLY;
    case FASTIX_SYS_getresuid:
        return sys_getres3(c, p->uid, p->euid, p->suid);
    case FASTIX_SYS_setgid:
        c->ret[0] = (uint64_t)(int64_t)proc_setgid(p, (uint32_t)c->arg[0]);
        return OUT_REPLY;
    case FASTIX_SYS_setegid:
        c->ret[0] = (uint64_t)(int64_t)proc_setegid(p, (uint32_t)c->arg[0]);
        return OUT_REPLY;
    case FASTIX_SYS_setregid:
        c->ret[0] = (uint64_t)(int64_t)proc_setregid(p, (uint32_t)c->arg[0], (uint32_t)c->arg[1]);
        return OUT_REPLY;
    case FASTIX_SYS_setresgid:
        c->ret[0] = (uint64_t)(int64_t)proc_setresgid(p, (uint32_t)c->arg[0], (uint32_t)c->arg[1],
                                                      (uint32_t)c->arg[2]);
        return OUT_REPLY;
    case FASTIX_SYS_getresgid:
        return sys_getres3(c, p->gid, p->egid, p->sgid);
    case FASTIX_SYS_umask:
        c->ret[0] = proc_umask(p, (uint32_t)c->arg[0]);
        return OUT_REPLY;
    case FASTIX_SYS_brk:
        vms_brk(p->vs, c->arg[0], &c->ret[0]);
        return OUT_REPLY;
    case FASTIX_SYS_mmap: /* addr, len, prot, flags, fd, offset */
        err = vms_mmap(p->vs, c->arg[0], c->arg[1], (uint32_t)c->arg[2], (uint32_t)c->arg[3],
                       &addr);
        c->ret[0] = err != 0 ? (uint64_t)(int64_t)err : addr;
        return OUT_REPLY;
    case FASTIX_SYS_munmap:
        c->ret[0] = (uint64_t)(int64_t)vms_munmap(p->vs, c->arg[0], c->arg[1]);
        return OUT_REPLY;
    case FASTIX_SYS_mprotect:
        c->ret[0] = (uint64_t)(int64_t)vms_mprotect(p->vs, c->arg[0], c->arg[1],
                                                    (uint32_t)c->arg[2]);
        return OUT_REPLY;
    case FASTIX_SYS_rt_sigaction:
        c->ret[0] = (uint64_t)(int64_t)sig_sys_action(p, (int)c->arg[0], c->arg[1], c->arg[2]);
        return OUT_REPLY;
    case FASTIX_SYS_rt_sigprocmask:
        c->ret[0] = (uint64_t)(int64_t)sig_sys_procmask(c->t, (int)c->arg[0], c->arg[1], c->arg[2]);
        return OUT_REPLY;
    case FASTIX_SYS_rt_sigpending:
        c->ret[0] = (uint64_t)(int64_t)sig_sys_pending(c->t, c->arg[0]);
        return OUT_REPLY;
    case FASTIX_SYS_rt_sigsuspend:
        return sys_sigsuspend(c);
    case FASTIX_SYS_rt_sigreturn:
        sig_sys_return(c->t, c->arg[0]);
        return OUT_NONE;
    case FASTIX_SYS_sigaltstack:
        c->ret[0] = (uint64_t)(int64_t)sig_sys_altstack(c->t, c->arg[0], c->arg[1]);
        return OUT_REPLY;
    case FASTIX_SYS_kill:
        c->ret[0] = (uint64_t)(int64_t)sig_sys_kill(p, (int32_t)c->arg[0], (int)c->arg[1]);
        return OUT_REPLY;
    case FASTIX_SYS_tkill:
        c->ret[0] = (uint64_t)(int64_t)sig_sys_tkill(p, (uint32_t)c->arg[0], (int)c->arg[1]);
        return OUT_REPLY;
    case FASTIX_SYS_tgkill:
        c->ret[0] = (uint64_t)(int64_t)sig_sys_tgkill(p, (uint32_t)c->arg[0], (uint32_t)c->arg[1],
                                                      (int)c->arg[2]);
        return OUT_REPLY;
    case FASTIX_SYS_rt_sigqueueinfo:
        c->ret[0] = (uint64_t)(int64_t)sig_sys_queueinfo(p, (uint32_t)c->arg[0], (int)c->arg[1],
                                                         c->arg[2]);
        return OUT_REPLY;
    case FASTIX_SYS_clock_gettime:
        return sys_clock_gettime(c);
    case FASTIX_SYS_nanosleep:
        return sys_nanosleep(c, FASTIX_CLOCK_MONOTONIC, 0, c->arg[0], c->arg[1]);
    case FASTIX_SYS_clock_nanosleep:
        return sys_nanosleep(c, (int)c->arg[0], c->arg[1], c->arg[2], c->arg[3]);
    case FASTIX_SYS_setitimer:
        c->ret[0] = (uint64_t)(int64_t)timer_sys_setitimer(p, (int)c->arg[0], c->arg[1], c->arg[2]);
        return OUT_REPLY;
    case FASTIX_SYS_getitimer:
        c->ret[0] = (uint64_t)(int64_t)timer_sys_getitimer(p, (int)c->arg[0], c->arg[1]);
        return OUT_REPLY;
    case FASTIX_SYS_timer_create:
        c->ret[0] = (uint64_t)(int64_t)timer_sys_create(p, (int)c->arg[0], c->arg[1], c->arg[2]);
        return OUT_REPLY;
    case FASTIX_SYS_timer_settime:
        c->ret[0] = (uint64_t)(int64_t)timer_sys_settime(p, (int32_t)c->arg[0], c->arg[1],
                                                         c->arg[2], c->arg[3]);
        return OUT_REPLY;
    case FASTIX_SYS_timer_gettime:
        c->ret[0] = (uint64_t)(int64_t)timer_sys_gettime(p, (int32_t)c->arg[0], c->arg[1]);
        return OUT_REPLY;
    case FASTIX_SYS_timer_getoverrun: {
        int32_t ov = 0;
        err = timer_sys_getoverrun(p, (int32_t)c->arg[0], &ov);
        c->ret[0] = err != 0 ? (uint64_t)(int64_t)err : (uint64_t)(int64_t)ov;
        return OUT_REPLY;
    }
    case FASTIX_SYS_timer_delete:
        c->ret[0] = (uint64_t)(int64_t)timer_sys_delete(p, (int32_t)c->arg[0]);
        return OUT_REPLY;
    case FASTIX_SYS_futex:
        return sys_futex(c);
    case FASTIX_SYS_set_robust_list:
        c->ret[0] = (uint64_t)(int64_t)futex_sys_set_robust_list(c->t, c->arg[0], c->arg[1]);
        return OUT_REPLY;
    case FASTIX_SYS_get_robust_list:
        c->ret[0] = (uint64_t)(int64_t)futex_sys_get_robust_list(c->t, (uint32_t)c->arg[0],
                                                                 c->arg[1], c->arg[2]);
        return OUT_REPLY;
    case FASTIX_SYS_pipe2: {
        int32_t fds[2] = { -1, -1 };
        err = pipe_sys_pipe2(p, c->arg[0], (uint32_t)c->arg[1], fds);
        c->ret[0] = (uint64_t)(int64_t)err;
        c->ret[1] = (uint64_t)(uint32_t)fds[0] | ((uint64_t)(uint32_t)fds[1] << 32);
        c->nret = 2;
        return OUT_REPLY;
    }
    case FASTIX_SYS_eventfd2:
        c->ret[0] = (uint64_t)(int64_t)pipe_sys_eventfd2(p, (uint32_t)c->arg[0],
                                                         (uint32_t)c->arg[1]);
        return OUT_REPLY;
    case FASTIX_SYS_close:
        c->ret[0] = (uint64_t)(int64_t)pipe_sys_close(p, (int32_t)c->arg[0]);
        return OUT_REPLY;
    case FASTIX_SYS_read:
        return sys_io_rw(c, false);
    case FASTIX_SYS_write:
        return sys_io_rw(c, true);
    case FASTIX_SYS_dup:
        c->ret[0] = (uint64_t)(int64_t)pipe_sys_dup(p, (int32_t)c->arg[0]);
        return OUT_REPLY;
    case FASTIX_SYS_dup3:
        c->ret[0] = (uint64_t)(int64_t)pipe_sys_dup3(p, (int32_t)c->arg[0], (int32_t)c->arg[1],
                                                     (uint32_t)c->arg[2]);
        return OUT_REPLY;
    case FASTIX_SYS_fcntl:
        c->ret[0] = (uint64_t)(int64_t)pipe_sys_fcntl(p, (int32_t)c->arg[0], (uint32_t)c->arg[1],
                                                      c->arg[2]);
        return OUT_REPLY;
    case FASTIX_SYS_sched_yield:
        seL4_Yield();
        return OUT_REPLY;
    default:
#ifdef FASTIX_TEST_HOOKS
        if (c->label >= FASTIX_IPC_LABEL_TEST_BASE) {
            return test_hook(c);
        }
#endif
        c->ret[0] = (uint64_t)-FASTIX_ENOSYS;
        return OUT_REPLY;
    }
}

static void kill_by_fault(struct proc *p, int sig, int code, uint64_t addr)
{
    last_kill.pid = p->pid;
    last_kill.sig = sig;
    last_kill.code = code;
    last_kill.addr = addr;
    kprint_str("procd: pid ");
    kprint_u64(p->pid);
    kprint_str(" killed by signal ");
    kprint_u64((uint64_t)sig);
    kprint_str(" at ");
    kprint_hex(addr);
    kprint_str("\n");
    proc_exit(p, FASTIX_W_SIGNALED(sig));
}

static enum outcome handle_fault(struct call *c)
{
    int sig = FASTIX_SIGSEGV, code = FASTIX_SEGV_MAPERR;
    uint64_t addr = 0;

    if (c->label == seL4_Fault_VMFault) {
        addr = seL4_GetMR(seL4_VMFault_Addr);
        enum vm_access access = VM_READ;
        if (seL4_GetMR(seL4_VMFault_PrefetchFault)) {
            access = VM_EXEC;
        } else if (arch_fault_is_write(seL4_GetMR(seL4_VMFault_FSR))) {
            access = VM_WRITE;
        }
        if (vms_fault(c->p->vs, addr, access, &sig, &code) == 0) {
            c->nret = 0; /* an empty reply resumes the faulting instruction */
            return OUT_REPLY;
        }
    } else if (c->label == seL4_Fault_UserException) {
        sig = FASTIX_SIGILL;
        code = FASTIX_ILL_ILLOPC;
        addr = seL4_GetMR(seL4_UserException_FaultIP);
    }
    if (sig_deliver_fault(c->t, sig, code, addr)) {
        return OUT_NONE; /* user signal handler is now running */
    }
    kill_by_fault(c->p, sig, code, addr);
    return OUT_NONE;
}

/* Handles one message. For OUT_REPLY the reply words are in the IPC buffer and *nret says how many. */
static enum outcome dispatch(seL4_Word badge, seL4_MessageInfo_t info, unsigned *nret)
{
    if (fastix_badge_type(badge) == FASTIX_BADGE_NOTIFY) {
        timer_tick();
        return OUT_NONE;
    }

    struct call c = { .label = seL4_MessageInfo_get_label(info), .nret = 1 };
    seL4_Word len = seL4_MessageInfo_get_length(info);

    c.t = proc_thread_from_badge(badge);
    if (c.t == NULL) {
        return OUT_NONE; /* sender is gone; nothing to answer */
    }
    c.p = c.t->proc;
    sig_set_active_caller(c.t);
    if (fastix_badge_type(badge) == FASTIX_BADGE_FAULT) {
        *nret = 0;
        enum outcome fo = handle_fault(&c);
        sig_set_active_caller(NULL);
        return fo;
    }
    c.p->ipc_count++;
    for (seL4_Word i = 0; i < len && i < CALL_MAX_ARGS; i++) {
        c.arg[i] = seL4_GetMR((int)i);
    }
    enum outcome o = OUT_REPLY;
    if (len > CALL_MAX_ARGS) {
        c.ret[0] = (uint64_t)-FASTIX_EINVAL;
    } else {
        o = handle_syscall(&c);
    }
    sig_set_active_caller(NULL);
    if (o == OUT_REPLY) {
        if (sig_check_on_return(c.t, c.nret, c.ret)) {
            return OUT_NONE; /* signal delivered on return path */
        }
        for (unsigned i = 0; i < c.nret; i++) {
            seL4_SetMR((int)i, c.ret[i]);
        }
        *nret = c.nret;
    }
    return o;
}

/* When PID 1 has ended, report its status and stop the machine. */
static void shutdown_if_init_exited(void)
{
    const struct proc *init = proc_get(1);
    if (init == NULL || !init->zombie) {
        return;
    }
    int sig = FASTIX_W_TERMSIG(init->status), code = FASTIX_W_EXITCODE(init->status);
    int rc = sig != 0 ? 128 + sig : code;
    kprint_str("procd: init exited, rc=");
    kprint_u64((uint64_t)rc);
    kprint_str(rc == 0 ? "\n[FASTIX-SUITE DONE rc=0]\n" : "\n[FASTIX-SUITE DONE rc=1]\n");
    arch_qemu_exit(rc == 0 ? 0 : 1);
    (void)seL4_TCB_Suspend(seL4_CapInitThreadTCB);
}

_Noreturn void server_run(void)
{
    bool have_reply = false;
    seL4_MessageInfo_t reply_info = seL4_MessageInfo_new(0, 0, 0, 0);

    for (;;) {
        seL4_Word badge;
        seL4_MessageInfo_t info =
            have_reply ? seL4_ReplyRecv(endpoint.cap, reply_info, &badge, current->obj.cap)
                       : seL4_Recv(endpoint.cap, &badge, current->obj.cap);
        unsigned nret = 0;
        have_reply = (dispatch(badge, info, &nret) == OUT_REPLY);
        reply_info = seL4_MessageInfo_new(0, 0, 0, nret);
        shutdown_if_init_exited();
    }
}
