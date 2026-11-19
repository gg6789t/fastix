/* proc_ids.c - process groups, sessions and credentials (split from proc.c). */
#include <arch/counter.h>
#include <arch/regs.h>
#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix/cspace.h>
#include <fastix/errno.h>
#include <fastix/futex_abi.h>
#include <fastix/mman.h>
#include <fastix/signal_abi.h>
#include <fastix/vdso.h>
#include <fastix/vspace.h>
#include <libos/assert.h>
#include <libos/slab.h>
#include "cspace.h"
#include "futex.h"
#include "heap.h"
#include "pipe.h"
#include "proc.h"
#include "proc_internal.h"
#include "signal.h"
#include "timer.h"
#include "vdso.h"

/* ---- process groups and sessions ---- */

static bool pgid_exists_in_session(uint32_t pgid, uint32_t sid)
{
    for (unsigned i = 0; i < FASTIX_MAX_PROCS; i++) {
        struct proc *p = proc_table_at(i);
        if (p != NULL && !p->zombie && p->pgid == pgid && p->sid == sid) {
            return true;
        }
    }
    return false;
}

static bool any_proc_has_pgid(uint32_t pgid)
{
    for (unsigned i = 0; i < FASTIX_MAX_PROCS; i++) {
        struct proc *p = proc_table_at(i);
        if (p != NULL && !p->zombie && p->pgid == pgid) {
            return true;
        }
    }
    return false;
}

int proc_getpgid(struct proc *caller, uint32_t pid, uint32_t *out)
{
    struct proc *target = pid == 0 ? caller : proc_get(pid);
    if (target == NULL || target->zombie) {
        return -FASTIX_ESRCH;
    }
    *out = target->pgid;
    return 0;
}

int proc_setpgid(struct proc *caller, uint32_t pid, uint32_t pgid)
{
    if ((int32_t)pid < 0 || (int32_t)pgid < 0) {
        return -FASTIX_EINVAL;
    }
    struct proc *target = pid == 0 ? caller : proc_get(pid);
    if (target == NULL || target->zombie) {
        return -FASTIX_ESRCH;
    }
    if (target != caller && target->ppid != caller->pid) {
        return -FASTIX_ESRCH;
    }
    if (target != caller && target->did_exec) {
        return -FASTIX_EACCES;
    }
    if (target->pid == target->sid || target->sid != caller->sid) {
        return -FASTIX_EPERM;
    }
    uint32_t new_pgid = pgid == 0 ? target->pid : pgid;
    if (new_pgid != target->pid && !pgid_exists_in_session(new_pgid, caller->sid)) {
        return -FASTIX_EPERM;
    }
    target->pgid = new_pgid;
    proc_sync_info(target);
    return 0;
}

int proc_getsid(struct proc *caller, uint32_t pid, uint32_t *out)
{
    struct proc *target = pid == 0 ? caller : proc_get(pid);
    if (target == NULL || target->zombie) {
        return -FASTIX_ESRCH;
    }
    *out = target->sid;
    return 0;
}

int proc_setsid(struct proc *caller, uint32_t *out)
{
    if (any_proc_has_pgid(caller->pid)) {
        return -FASTIX_EPERM;
    }
    caller->sid = caller->pid;
    caller->pgid = caller->pid;
    proc_sync_info(caller);
    *out = caller->sid;
    return 0;
}

/* ---- credentials & umask ---- */

static bool cred_allowed(uint32_t val, uint32_t real, uint32_t eff, uint32_t saved)
{
    return val == CRED_UNCHANGED || val == real || val == eff || val == saved;
}

int proc_setuid(struct proc *p, uint32_t uid)
{
    if (p->euid == 0) {
        p->uid = uid;
        p->euid = uid;
        p->suid = uid;
    } else if (uid == p->uid || uid == p->suid) {
        p->euid = uid;
    } else {
        return -FASTIX_EPERM;
    }
    proc_sync_info(p);
    return 0;
}

int proc_seteuid(struct proc *p, uint32_t euid)
{
    if (p->euid != 0 && euid != p->uid && euid != p->euid && euid != p->suid) {
        return -FASTIX_EPERM;
    }
    p->euid = euid;
    proc_sync_info(p);
    return 0;
}

int proc_setreuid(struct proc *p, uint32_t ruid, uint32_t euid)
{
    if (p->euid != 0) {
        if ((ruid != CRED_UNCHANGED && ruid != p->uid && ruid != p->euid) ||
            !cred_allowed(euid, p->uid, p->euid, p->suid)) {
            return -FASTIX_EPERM;
        }
    }
    uint32_t old_uid = p->uid;
    if (ruid != CRED_UNCHANGED) {
        p->uid = ruid;
    }
    if (euid != CRED_UNCHANGED) {
        p->euid = euid;
    }
    if (ruid != CRED_UNCHANGED || (euid != CRED_UNCHANGED && euid != old_uid)) {
        p->suid = p->euid;
    }
    proc_sync_info(p);
    return 0;
}

int proc_setresuid(struct proc *p, uint32_t ruid, uint32_t euid, uint32_t suid)
{
    if (p->euid != 0) {
        if (!cred_allowed(ruid, p->uid, p->euid, p->suid) ||
            !cred_allowed(euid, p->uid, p->euid, p->suid) ||
            !cred_allowed(suid, p->uid, p->euid, p->suid)) {
            return -FASTIX_EPERM;
        }
    }
    if (ruid != CRED_UNCHANGED) {
        p->uid = ruid;
    }
    if (euid != CRED_UNCHANGED) {
        p->euid = euid;
    }
    if (suid != CRED_UNCHANGED) {
        p->suid = suid;
    }
    proc_sync_info(p);
    return 0;
}

int proc_setgid(struct proc *p, uint32_t gid)
{
    if (p->euid == 0) {
        p->gid = gid;
        p->egid = gid;
        p->sgid = gid;
    } else if (gid == p->gid || gid == p->sgid) {
        p->egid = gid;
    } else {
        return -FASTIX_EPERM;
    }
    proc_sync_info(p);
    return 0;
}

int proc_setegid(struct proc *p, uint32_t egid)
{
    if (p->euid != 0 && egid != p->gid && egid != p->egid && egid != p->sgid) {
        return -FASTIX_EPERM;
    }
    p->egid = egid;
    proc_sync_info(p);
    return 0;
}

int proc_setregid(struct proc *p, uint32_t rgid, uint32_t egid)
{
    if (p->euid != 0) {
        if ((rgid != CRED_UNCHANGED && rgid != p->gid && rgid != p->egid) ||
            !cred_allowed(egid, p->gid, p->egid, p->sgid)) {
            return -FASTIX_EPERM;
        }
    }
    uint32_t old_gid = p->gid;
    if (rgid != CRED_UNCHANGED) {
        p->gid = rgid;
    }
    if (egid != CRED_UNCHANGED) {
        p->egid = egid;
    }
    if (rgid != CRED_UNCHANGED || (egid != CRED_UNCHANGED && egid != old_gid)) {
        p->sgid = p->egid;
    }
    proc_sync_info(p);
    return 0;
}

int proc_setresgid(struct proc *p, uint32_t rgid, uint32_t egid, uint32_t sgid)
{
    if (p->euid != 0) {
        if (!cred_allowed(rgid, p->gid, p->egid, p->sgid) ||
            !cred_allowed(egid, p->gid, p->egid, p->sgid) ||
            !cred_allowed(sgid, p->gid, p->egid, p->sgid)) {
            return -FASTIX_EPERM;
        }
    }
    if (rgid != CRED_UNCHANGED) {
        p->gid = rgid;
    }
    if (egid != CRED_UNCHANGED) {
        p->egid = egid;
    }
    if (sgid != CRED_UNCHANGED) {
        p->sgid = sgid;
    }
    proc_sync_info(p);
    return 0;
}

uint32_t proc_umask(struct proc *p, uint32_t mask)
{
    uint32_t old = p->umask;
    p->umask = mask & 0777u;
    proc_sync_info(p);
    return old;
}
