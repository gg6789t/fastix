/* exec.c - execve and posix_spawn_fast (see exec.h). */
#include <stddef.h>
#include <stdint.h>

#include <arch/counter.h>
#include <arch/regs.h>
#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/errno.h>
#include <fastix/signal_abi.h>
#include <fastix/vspace.h>
#include <libos/mem.h>

#include "exec.h"
#include "images.h"
#include "loader.h"
#include "pipe.h"
#include "signal.h"
#include "timer.h"
#include "vdso.h"


/* Single-threaded procd scratch buffers (keeps server stack tiny). */
struct exec_buf {
    char pool[FASTIX_ARG_MAX];
    size_t used;
    const char *argv[FASTIX_EXEC_MAX_ARGS + 1];
    size_t argc;
    const char *envp[FASTIX_EXEC_MAX_ARGS + 1];
    size_t envc;
    char path[FASTIX_PATH_MAX];
};

static struct exec_buf ebuf;

static char *pool_dup(struct exec_buf *b, const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    if (n + 1 > sizeof(b->pool) - b->used) {
        return NULL;
    }
    char *dst = b->pool + b->used;
    memcpy(dst, s, n + 1);
    b->used += n + 1;
    return dst;
}

/* Copies a NULL-terminated user string array into `out[0..max-1]`, storing count in *count. */
static int copy_str_vec(struct vmspace *vs, uint64_t vec_u, struct exec_buf *b,
                        const char **out, size_t max, size_t *count)
{
    *count = 0;
    if (vec_u == 0) {
        out[0] = NULL;
        return 0;
    }
    for (size_t i = 0; i < max; i++) {
        uint64_t ptr = 0;
        int err = vms_read(vs, vec_u + i * sizeof(uint64_t), &ptr, sizeof(ptr));
        if (err != 0) {
            return err;
        }
        if (ptr == 0) {
            out[i] = NULL;
            *count = i;
            return 0;
        }
        size_t avail = sizeof(b->pool) - b->used;
        if (avail == 0) {
            return -FASTIX_E2BIG;
        }
        char *dst = b->pool + b->used;
        err = vms_read_str(vs, ptr, dst, avail);
        if (err == -FASTIX_ENAMETOOLONG) {
            return -FASTIX_E2BIG;
        }
        if (err != 0) {
            return err;
        }
        size_t len = 0;
        while (dst[len] != '\0') {
            len++;
        }
        b->used += len + 1;
        out[i] = dst;
    }
    return -FASTIX_E2BIG;
}

/* Prepends interpreter (and optional single arg) and replaces argv[0] with script path. */
static int prepend_shebang(struct exec_buf *b, const char *interp, const char *opt_arg,
                           const char *script_path)
{
    size_t add = (opt_arg[0] != '\0') ? 2u : 1u;
    size_t tail = b->argc > 0 ? b->argc - 1u : 0u; /* drop previous argv[0] */
    if (add + 1u + tail > FASTIX_EXEC_MAX_ARGS) {
        return -FASTIX_E2BIG;
    }
    char *s_interp = pool_dup(b, interp);
    char *s_script = pool_dup(b, script_path);
    char *s_opt = (opt_arg[0] != '\0') ? pool_dup(b, opt_arg) : NULL;
    if (s_interp == NULL || s_script == NULL || (opt_arg[0] != '\0' && s_opt == NULL)) {
        return -FASTIX_E2BIG;
    }
    /* Shift tail right to make room for [interp, (opt_arg), script_path]. */
    for (size_t i = tail; i > 0; i--) {
        b->argv[add + 1u + (i - 1u)] = b->argv[1u + (i - 1u)];
    }
    b->argv[0] = s_interp;
    if (s_opt != NULL) {
        b->argv[1] = s_opt;
        b->argv[2] = s_script;
    } else {
        b->argv[1] = s_script;
    }
    b->argc = add + 1u + tail;
    b->argv[b->argc] = NULL;
    return 0;
}

/* Resolves path (following up to FASTIX_SHEBANG_MAX_DEPTH #! scripts) to an ELF image. */
static int resolve_image(struct exec_buf *b, const struct image **out)
{
    char interp[FASTIX_PATH_MAX];
    char opt_arg[256];
    const char *cur_path = b->path;

    for (unsigned depth = 0;; depth++) {
        const struct image *img = image_find(cur_path);
        if (img == NULL) {
            return -FASTIX_ENOENT;
        }
        size_t size = (size_t)(img->end - img->start);
        int sb = loader_parse_shebang(img->start, size, interp, sizeof(interp), opt_arg,
                                      sizeof(opt_arg));
        if (sb < 0) {
            return sb;
        }
        if (sb == 0) {
            *out = img;
            return 0;
        }
        if (depth >= FASTIX_SHEBANG_MAX_DEPTH) {
            return -FASTIX_ELOOP;
        }
        int err = prepend_shebang(b, interp, opt_arg, cur_path);
        if (err != 0) {
            return err;
        }
        cur_path = b->argv[0];
    }
}

static int read_exec_inputs(struct vmspace *vs, uint64_t path_u, uint64_t argv_u, uint64_t envp_u,
                            struct exec_buf *b, const struct image **img_out)
{
    b->used = 0;
    b->argc = 0;
    b->envc = 0;
    int err = vms_read_str(vs, path_u, b->path, sizeof(b->path));
    if (err != 0) {
        return err;
    }
    if ((err = copy_str_vec(vs, argv_u, b, b->argv, FASTIX_EXEC_MAX_ARGS, &b->argc)) != 0 ||
        (err = copy_str_vec(vs, envp_u, b, b->envp, FASTIX_EXEC_MAX_ARGS, &b->envc)) != 0) {
        return err;
    }
    return resolve_image(b, img_out);
}

int proc_execve(struct thread *caller, uint64_t path_u, uint64_t argv_u, uint64_t envp_u)
{
    struct proc *p = caller->proc;
    const struct image *img;
    int err = read_exec_inputs(p->vs, path_u, argv_u, envp_u, &ebuf, &img);
    if (err != 0) {
        return err;
    }

    /* Build the new address space first so any error leaves caller completely intact. */
    struct vmspace *new_vs = NULL;
    struct frame *new_ipcbuf = NULL;
    uint64_t entry = 0, sp = 0;

    err = vms_create(&new_vs);
    if (err == 0 && p->vfork_parent_pid != 0) {
        /* vfork child borrowed parent's IPC buffer; give it its own now. */
        err = frame_new(&new_ipcbuf);
    }
    struct frame *use_ipcbuf = new_ipcbuf != NULL ? new_ipcbuf : caller->ipcbuf;
    if (err == 0) {
        err = proc_map_special_pages(p, new_vs, use_ipcbuf);
    }
    if (err == 0) {
        struct load_args la = {
            .elf = img->start,
            .elf_size = (size_t)(img->end - img->start),
            .argv = ebuf.argv,
            .envp = ebuf.envp,
            .random_seed = arch_read_counter() ^ p->pid,
            .uid = p->uid,
            .euid = p->euid,
            .gid = p->gid,
            .egid = p->egid,
            .vdso_ehdr = FASTIX_VDSO_CODE_ADDR,
        };
        err = loader_load(new_vs, &la, &entry, &sp);
    }
    if (err != 0) {
        if (new_ipcbuf != NULL) {
            frame_put(new_ipcbuf);
        }
        vms_put(new_vs);
        return err;
    }

    /* Point of no return: commit the new address space and restart caller at entry. */
    (void)seL4_TCB_Suspend(caller->tcb.cap);
    proc_kill_other_threads(caller);
    if (new_ipcbuf != NULL) {
        frame_put(caller->ipcbuf);
        caller->ipcbuf = new_ipcbuf;
    }
    err = proc_tcb_set_space(p, caller, new_vs);
    if (err == 0) {
        err = kobj_sel4_err(seL4_TCB_SetIPCBuffer(caller->tcb.cap, FASTIX_IPCBUF_BASE,
                                                  caller->ipcbuf->cap));
    }
    if (err != 0) {
        vms_put(new_vs);
        proc_exit(p, FASTIX_W_SIGNALED(FASTIX_SIGKILL));
        return 0;
    }

    struct vmspace *old_vs = p->vs;
    p->vs = new_vs;
    p->did_exec = true;
    vms_put(old_vs);
    sig_on_exec(p, caller);
    timer_proc_cleanup(p);
    pipe_proc_exec(p);
    proc_sync_info(p);

    /* Unblock vfork parent if this process was its vfork child. */
    proc_vfork_wake_parent(p);

    seL4_UserContext regs;
    arch_regs_init_entry(&regs, entry, sp, 0);
    err = proc_thread_start(caller, &regs);
    if (err != 0) {
        proc_exit(p, FASTIX_W_SIGNALED(FASTIX_SIGKILL));
    }
    return 0;
}

int proc_posix_spawn(struct thread *caller, uint64_t args_u, uint32_t *child_pid)
{
    struct proc *parent = caller->proc;
    struct fastix_spawn_args sa;
    int err = vms_read(parent->vs, args_u, &sa, sizeof(sa));
    if (err != 0) {
        return err;
    }
    uint32_t allowed = FASTIX_SPAWN_SETPGROUP | FASTIX_SPAWN_SETSIGDEF | FASTIX_SPAWN_SETSIGMASK;
    if ((sa.flags & ~allowed) != 0) {
        return -FASTIX_EINVAL;
    }

    const struct image *img;
    err = read_exec_inputs(parent->vs, sa.path, sa.argv, sa.envp, &ebuf, &img);
    if (err != 0) {
        return err;
    }

    struct proc *child;
    err = proc_create_empty(parent, parent->pid, &child);
    if (err != 0) {
        return err;
    }
    if (sa.flags & FASTIX_SPAWN_SETPGROUP) {
        err = proc_setpgid(parent, child->pid, sa.pgroup);
        if (err != 0) {
            proc_discard(child);
            return err;
        }
    }
    sig_thread_init(child->main_thread, caller);
    if (sa.flags & FASTIX_SPAWN_SETSIGMASK) {
        uint64_t uncatch = fastix_sig_bit(FASTIX_SIGKILL) | fastix_sig_bit(FASTIX_SIGSTOP);
        child->main_thread->sigmask = sa.sigmask & ~uncatch;
    }
    if (sa.flags & FASTIX_SPAWN_SETSIGDEF) {
        for (int s = 1; s < FASTIX_NSIG; s++) {
            if (sa.sigdefault & fastix_sig_bit(s)) {
                child->sigact[s] = (struct fastix_sigaction){ 0 };
            }
        }
    }
    sig_on_exec(child, child->main_thread);
    pipe_proc_exec(child);

    struct load_args la = {
        .elf = img->start,
        .elf_size = (size_t)(img->end - img->start),
        .argv = ebuf.argv,
        .envp = ebuf.envp,
        .random_seed = arch_read_counter() ^ child->pid,
        .uid = child->uid,
        .euid = child->euid,
        .gid = child->gid,
        .egid = child->egid,
        .vdso_ehdr = FASTIX_VDSO_CODE_ADDR,
    };
    uint64_t entry = 0, sp = 0;
    if ((err = loader_load(child->vs, &la, &entry, &sp)) == 0) {
        child->did_exec = true;
        seL4_UserContext regs;
        arch_regs_init_entry(&regs, entry, sp, 0);
        err = proc_thread_start(child->main_thread, &regs);
    }
    if (err != 0) {
        proc_discard(child);
        return err;
    }
    *child_pid = child->pid;
    return 0;
}
