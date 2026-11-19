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

#include "images.h"
#include "proc.h"
#include "reply.h"
#include "server.h"
#include "vdso.h"

#define CALL_MAX_ARGS 6
#define CALL_MAX_RET  7

enum outcome {
    OUT_REPLY,  /* answer now with ret[0..nret-1] */
    OUT_PARKED, /* the caller waits; its Reply object was parked */
    OUT_NONE,   /* no answer: the caller no longer exists */
};

struct call {
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

/* Keeps the caller's Reply object with it and takes a fresh one for the loop. */
static int park(struct proc *p, int32_t wait_pid)
{
    struct reply_obj *fresh;
    int err = reply_get(&fresh);
    if (err != 0) {
        return err;
    }
    p->wait_reply = current;
    p->wait_pid = wait_pid;
    current = fresh;
    return 0;
}

static enum outcome sys_wait4(struct call *c)
{
    uint32_t child;
    int status;
    int r = proc_wait(c->p, (int32_t)c->arg[0], c->arg[2], &child, &status);

    if (r == 1) {
        int err = park(c->p, (int32_t)c->arg[0]);
        if (err == 0) {
            return OUT_PARKED;
        }
        r = err;
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
    int err = proc_fork(c->p, &child);
    c->ret[0] = err != 0 ? (uint64_t)(int64_t)err : child->pid;
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
        c->ret[1] = c->p->vs.vmas.count;
        c->ret[2] = c->p->vs.pages.count;
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
        struct load_args la = { img->start, (size_t)(img->end - img->start), argv, envp,
                                arch_read_counter() ^ c->p->pid };
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
    int err;

    switch (c->label) {
    case FASTIX_SYS_exit:
    case FASTIX_SYS_exit_group:
        proc_exit(p, FASTIX_W_EXITED((int)c->arg[0]));
        return OUT_NONE;
    case FASTIX_SYS_getpid:
        c->ret[0] = p->pid;
        return OUT_REPLY;
    case FASTIX_SYS_getppid:
        c->ret[0] = p->ppid;
        return OUT_REPLY;
    case FASTIX_SYS_brk:
        vms_brk(&p->vs, c->arg[0], &c->ret[0]);
        return OUT_REPLY;
    case FASTIX_SYS_mmap: /* addr, len, prot, flags, fd, offset */
        err = vms_mmap(&p->vs, c->arg[0], c->arg[1], (uint32_t)c->arg[2], (uint32_t)c->arg[3],
                       &addr);
        c->ret[0] = err != 0 ? (uint64_t)(int64_t)err : addr;
        return OUT_REPLY;
    case FASTIX_SYS_munmap:
        c->ret[0] = (uint64_t)(int64_t)vms_munmap(&p->vs, c->arg[0], c->arg[1]);
        return OUT_REPLY;
    case FASTIX_SYS_mprotect:
        c->ret[0] = (uint64_t)(int64_t)vms_mprotect(&p->vs, c->arg[0], c->arg[1],
                                                    (uint32_t)c->arg[2]);
        return OUT_REPLY;
    case FASTIX_SYS_fork:
        return sys_fork(c);
    case FASTIX_SYS_wait4: /* pid, status pointer (unused: status comes back in MR1), options */
        return sys_wait4(c);
    case FASTIX_SYS_clock_gettime:
        return sys_clock_gettime(c);
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
        if (vms_fault(&c->p->vs, addr, access, &sig, &code) == 0) {
            c->nret = 0; /* an empty reply resumes the faulting instruction */
            return OUT_REPLY;
        }
    } else if (c->label == seL4_Fault_UserException) {
        sig = FASTIX_SIGILL;
        code = FASTIX_ILL_ILLOPC;
        addr = seL4_GetMR(seL4_UserException_FaultIP);
    }
    kill_by_fault(c->p, sig, code, addr);
    return OUT_NONE;
}

/* Handles one message. For OUT_REPLY the reply words are in the IPC buffer and *nret says how many. */
static enum outcome dispatch(seL4_Word badge, seL4_MessageInfo_t info, unsigned *nret)
{
    struct call c = { .label = seL4_MessageInfo_get_label(info), .nret = 1 };
    seL4_Word len = seL4_MessageInfo_get_length(info);

    c.p = proc_from_badge(badge);
    if (c.p == NULL) {
        return OUT_NONE; /* sender is gone; nothing to answer */
    }
    if (fastix_badge_type(badge) == FASTIX_BADGE_FAULT) {
        *nret = 0;
        return handle_fault(&c);
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
    if (o == OUT_REPLY) {
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
