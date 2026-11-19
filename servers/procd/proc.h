/*
 * proc.h - processes and threads: table, spawn, fork, vfork, exit, wait,
 * process groups, sessions, and credentials.
 *
 * Every process has at least one struct thread (its main thread, tid == pid,
 * tid_slot == 0). Multi-threading (Step 3C) adds more threads to proc->threads.
 */
#ifndef PROCD_PROC_H
#define PROCD_PROC_H

#include <stdbool.h>
#include <stdint.h>

#include <fastix/config.h>
#include <fastix/signal_abi.h>
#include <libos/compiler.h>
#include <libos/list.h>
#include <sel4/sel4.h>

#include "frame.h"
#include "kobj.h"
#include "loader.h"
#include "reply.h"
#include "vmspace.h"

struct proc;
struct fd_obj;

/* TEMPORARY Phase 3 FD table for pipe/eventfd only; moves to vfsd in Phase 4 (AGENTS.md 6.4). */
#define PROCD_TEMP_MAX_FDS 64

struct temp_fd {
    struct fd_obj *obj;
    uint32_t fd_flags; /* FASTIX_FD_CLOEXEC */
};

enum park_kind {
    PARK_NONE = 0,
    PARK_WAIT4,
    PARK_VFORK,
    PARK_SLEEP,
    PARK_SIGSUSPEND,
    PARK_FUTEX,
    PARK_PIPE_READ,
    PARK_PIPE_WRITE,
    PARK_EFD_READ,
    PARK_EFD_WRITE,
};

struct futex_key {
    uint64_t a;
    uint64_t b;
};

struct thread {
    struct list_node link; /* in proc->threads */
    struct proc *proc;
    uint32_t tid;          /* main thread: tid == proc->pid */
    uint16_t tid_slot;     /* 0 = main thread, 1..1023 = worker threads */
    bool exited;
    struct kobj tcb;
    struct kobj sc;
    seL4_CPtr fault_ep;    /* badged fault endpoint copy in procd's CNode */
    struct frame *ipcbuf;  /* per-thread IPC buffer frame */

    struct reply_obj *park_reply;
    enum park_kind park_kind;
    int32_t wait_pid;
    uint64_t wait_options;
    uint64_t wait_status_ptr;

    /* Per-thread signal state (Step 3B). */
    uint64_t sigmask;
    uint64_t saved_sigmask;
    bool has_saved_sigmask;
    uint64_t pending_mask;
    struct list_node sigq;
    uint32_t sigq_count;
    struct fastix_sigaltstack altstack;

    /* Parked nanosleep / clock_nanosleep state (Step 3B). */
    struct list_node sleep_link;
    uint64_t sleep_deadline_ns;
    uint64_t sleep_rem_ptr;
    int sleep_clock;
    bool sleep_abstime;

    /* Per-thread futex & thread-exit state (Step 3C). */
    uint64_t clear_child_tid;
    uint64_t robust_list;
    struct list_node futex_link;
    struct futex_key futex_key;
    uint32_t futex_bitset;
    uint64_t futex_deadline_ns;
    int futex_clock;

    /* Parked pipe/eventfd I/O waiter state (Step 3D). */
    struct list_node io_link;
    struct fd_obj *io_obj;
    uint64_t io_buf_u;
    uint64_t io_len;
    uint64_t io_done;
    uint64_t io_efd_val;
};

struct proc {
    uint32_t pid;
    uint16_t gen;          /* badge generation: stale badges never match */
    uint32_t ppid;         /* 0 for PID 1 */
    uint32_t pgid;
    uint32_t sid;
    uint32_t uid, euid, suid;
    uint32_t gid, egid, sgid;
    uint32_t umask;

    bool zombie;
    bool did_exec;
    bool stopped;
    bool stop_reported;
    bool cont_pending;
    int stop_sig;
    int status;            /* wait status, once a zombie */

    struct vmspace *vs;    /* refcounted address space (shared during vfork) */
    struct kobj cnode;     /* root CNode (64 slots) */
    struct kobj thread_cnode; /* lazy level-2 CNode for worker thread caps (slot 63) */
    struct frame *procinfo;

    struct list_node threads;
    struct thread *main_thread;
    uint32_t live_threads;
    uint64_t tid_slots[FASTIX_MAX_THREADS_PER_PROC / 64];

    /* Per-process signal state (Step 3B). */
    struct fastix_sigaction sigact[FASTIX_NSIG];
    uint64_t pending_mask;
    struct list_node sigq;
    uint32_t sigq_count;

    /* Per-process timers (Step 3B). */
    uint64_t itimer_deadline_ns;
    uint64_t itimer_interval_ns;
    struct list_node posix_timers;

    /* TEMPORARY Phase 3 FD table for pipe/eventfd only (moves to vfsd in Phase 4). */
    struct temp_fd temp_fds[PROCD_TEMP_MAX_FDS];

    /* Non-zero while this child was created by vfork() and still shares parent's vs. */
    uint32_t vfork_parent_pid;

    uint64_t ipc_count;    /* calls received from this process (tests) */
};

void proc_module_init(seL4_CPtr endpoint, seL4_CPtr sched_control);

/* Live or zombie process with this pid, or NULL. */
struct proc *proc_get(uint32_t pid);

/* Live thread matching a procd syscall/fault badge, or NULL. */
struct thread *proc_thread_from_badge(uint64_t badge);

/* Updates the read-only procinfo page to match p's current fields. */
void proc_sync_info(struct proc *p);

/* Maps VDSO, procinfo, and main-thread IPC buffer into `vs`. */
LIBOS_MUST_CHECK int proc_map_special_pages(struct proc *p, struct vmspace *vs,
                                            struct frame *ipcbuf);

/* Sets root CSpace + VSpace + fault_ep on a thread's TCB. */
LIBOS_MUST_CHECK int proc_tcb_set_space(struct proc *p, struct thread *t, struct vmspace *vs);

/* Writes initial registers (with tpidrro_el0 = t->tid_slot) and resumes `t`. */
LIBOS_MUST_CHECK int proc_thread_start(struct thread *t, seL4_UserContext *regs);

/* Detaches a vfork child from its parent's vmspace and wakes the parent. */
void proc_vfork_wake_parent(struct proc *child);

/* Allocates and builds a new process shell (empty vmspace) inheriting from `parent` (or defaults if NULL). */
LIBOS_MUST_CHECK int proc_create_empty(struct proc *parent, uint32_t ppid, struct proc **out);

/* Discards a partly-built process that failed before starting. */
void proc_discard(struct proc *p);

/* Creates a process running the given program. Returns 0 or -errno. */
LIBOS_MUST_CHECK int proc_spawn(const struct load_args *la, uint32_t ppid, struct proc **out);

/* Creates a copy-on-write copy of `caller->proc`. Child starts immediately with fork() == 0. */
LIBOS_MUST_CHECK int proc_fork(struct thread *caller, struct proc **out);

/*
 * Creates a vfork child sharing `caller->proc->vs`. Caller is parked on `parent_reply`
 * until the child calls execve() or exits. Returns 0 on success (caller parked) or -errno.
 */
LIBOS_MUST_CHECK int proc_vfork(struct thread *caller, struct reply_obj *parent_reply,
                                struct proc **out);

/* Ends a live process with wait status `status`. */
void proc_exit(struct proc *p, int status);

/* Creates a new thread inside `caller->proc`. Returns 0 and new tid in *tid_out, or -errno. */
LIBOS_MUST_CHECK int proc_thread_create(struct thread *caller, uint64_t entry, uint64_t stack,
                                        uint64_t arg, uint64_t tls, uint64_t ctid_ptr,
                                        uint32_t flags, uint32_t *tid_out);

/* Exits only thread `t` (runs robust list & clear_child_tid; exits process if last thread). */
void proc_thread_exit(struct thread *t, int code);

/* Terminates every thread of `caller->proc` except `caller` (used by execve). */
void proc_kill_other_threads(struct thread *caller);

/*
 * wait4 for `p`: pid -1 = any child, pid > 0 = that child, pid == 0 = caller's pgid,
 * pid < -1 = pgid == -pid.
 * Returns 0 with *child_pid and *status set (child_pid 0 if WNOHANG and none ready),
 * 1 if the caller must block, or -errno.
 */
int proc_wait(struct proc *p, int32_t pid, uint64_t options, uint32_t *child_pid, int *status);

/* Wakes any thread of `parent` parked in wait4 if a matching child state is ready. */
void proc_wake_waiter(struct proc *parent);

/* Process groups and sessions. */
int proc_getpgid(struct proc *caller, uint32_t pid, uint32_t *out);
int proc_setpgid(struct proc *caller, uint32_t pid, uint32_t pgid);
int proc_getsid(struct proc *caller, uint32_t pid, uint32_t *out);
int proc_setsid(struct proc *caller, uint32_t *out);

/* Credentials and umask. */
int proc_setuid(struct proc *p, uint32_t uid);
int proc_seteuid(struct proc *p, uint32_t euid);
int proc_setreuid(struct proc *p, uint32_t ruid, uint32_t euid);
int proc_setresuid(struct proc *p, uint32_t ruid, uint32_t euid, uint32_t suid);
int proc_setgid(struct proc *p, uint32_t gid);
int proc_setegid(struct proc *p, uint32_t egid);
int proc_setregid(struct proc *p, uint32_t rgid, uint32_t egid);
int proc_setresgid(struct proc *p, uint32_t rgid, uint32_t egid, uint32_t sgid);
uint32_t proc_umask(struct proc *p, uint32_t mask);

/* Processes in the table (live + zombie). */
uint64_t proc_count(void);

/* Process at table slot `idx` (0..FASTIX_MAX_PROCS-1), or NULL. */
struct proc *proc_table_at(unsigned idx);

/* Finds a live thread with `tid` in `p`, or NULL. */
struct thread *proc_find_thread(struct proc *p, uint32_t tid);

/* Finds a live thread with `tid` across all processes, or NULL. */
struct thread *proc_find_thread_global(uint32_t tid);

#endif /* PROCD_PROC_H */
