/*
 * client.h - vfsd's clients (one per process), their FD tables and the open file
 * descriptions (OFDs) behind them (AGENTS.md 6.4). Every FD change is mirrored into the
 * client's exported FD table, which procd reads.
 */
#ifndef VFSD_CLIENT_H
#define VFSD_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include <fastix/config.h>
#include <libos/compiler.h>

#include "vnode.h"

/* One open file description, shared by dup'ed and inherited FDs. */
struct ofd {
    uint32_t refs;
    uint32_t flags; /* O_ACCMODE | O_APPEND | O_NONBLOCK */
    uint32_t owner; /* FASTIX_OWNER_VFSD or FASTIX_OWNER_PROCD */
    uint32_t obj;   /* procd object index (owner PROCD) */
    uint32_t kind;  /* procd object kind, FASTIX_OBJ_* (owner PROCD) */
    uint64_t pos;   /* file offset, or readdir cookie for directories */
    struct vnode *v; /* the open vnode (owner VFSD), one reference */
};

struct client {
    bool used;
    uint32_t pid;
    uint32_t slot;
    struct vnode *cwd; /* one reference */
    struct ofd *fds[FASTIX_DEFAULT_NOFILE];
    uint64_t cloexec[FASTIX_DEFAULT_NOFILE / 64];
};

LIBOS_MUST_CHECK int clients_init(struct vnode *root);

/* The client that sent a call with this badge, or NULL. */
struct client *client_from_badge(uint64_t badge);

/* Effective (and real) credentials and umask, read from the client's procinfo page. */
struct cred client_cred(const struct client *c);
struct cred client_real_cred(const struct client *c);
uint32_t client_umask(const struct client *c);

/* The root directory every client sees (no chroot yet). */
struct vnode *vfs_root(void);

/* Runs one procd work item; fd0 and fd1 receive the FDs of an INSTALL. */
int64_t client_work(uint32_t kind, uint32_t slot, uint32_t pid, const uint64_t a[4],
                    int32_t *fd0, int32_t *fd1);

/* OFDs */
struct ofd *ofd_new_vnode(struct vnode *v, uint32_t flags); /* takes the caller's v reference */
void ofd_put(struct ofd *o);
struct ofd *fd_get(const struct client *c, int64_t fd); /* NULL if fd is not open */

/* Puts `o` at the lowest free FD >= min_fd (takes the caller's reference). fd or -EMFILE. */
int fd_install(struct client *c, struct ofd *o, int64_t min_fd, bool cloexec);

/* FD syscalls handled entirely here. */
int64_t sys_close(struct client *c, int64_t fd);
int64_t sys_dup(struct client *c, int64_t fd);
int64_t sys_dup3(struct client *c, int64_t oldfd, int64_t newfd, uint64_t flags);
int64_t sys_fcntl(struct client *c, int64_t fd, uint64_t cmd, uint64_t arg);

/* Live clients and OFDs (leak checks). */
uint64_t client_count(void);
uint64_t ofd_count(void);

#endif /* VFSD_CLIENT_H */
