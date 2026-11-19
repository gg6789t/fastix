/*
 * tmpfs.h - in-memory filesystem: directories, regular files (sparse, page by page),
 * symlinks and hard links. File data lives in vfsd memory until the page cache (4E).
 */
#ifndef VFSD_TMPFS_H
#define VFSD_TMPFS_H

#include <stdint.h>

#include <libos/compiler.h>

#include "vnode.h"

/* Largest file size tmpfs accepts (larger writes and truncates fail with EFBIG). */
#define TMPFS_MAX_FILE_SIZE (1ULL << 40)

void tmpfs_module_init(void);

/* Creates an empty tmpfs whose root directory has `mode` (permission bits) and owner `cr`.
 * Data pages are limited to `max_bytes` (ENOSPC beyond). *root gets one reference and is not
 * mounted yet. */
LIBOS_MUST_CHECK int tmpfs_new(uint64_t max_bytes, uint32_t mode, const struct cred *cr,
                               struct vnode **root);

/* Live tmpfs nodes and data pages in all tmpfs instances (leak checks). */
struct tmpfs_stats {
    uint64_t nodes;
    uint64_t pages;
};
struct tmpfs_stats tmpfs_stats(void);

#endif /* VFSD_TMPFS_H */
