/*
 * libos/slab.h - fixed-size object allocator.
 * One cache per object type. A cache carves whole pages (from a provider)
 * into equal objects and keeps freed objects on a free list for reuse.
 * Pages are kept by the cache once taken (simple and fast; no shrinking yet).
 */
#ifndef LIBOS_SLAB_H
#define LIBOS_SLAB_H

#include <stddef.h>
#include <stdint.h>

#include <libos/compiler.h>

#define SLAB_ALIGN 16 /* every object is 16-byte aligned */

struct slab_provider {
    /* Stores a zeroed, page-aligned page of FASTIX_PAGE_SIZE bytes in *page. Returns 0 or -errno. */
    int (*get_page)(void *ctx, void **page);
    void *ctx;
};

struct slab_free; /* free objects link through their own first bytes */

struct slab_cache {
    const char *name;
    size_t obj_size;          /* rounded up to SLAB_ALIGN */
    struct slab_free *free;   /* free objects */
    uint64_t in_use;          /* objects handed out right now */
    uint64_t pages;           /* pages taken from the provider */
    struct slab_provider provider;
};

/* obj_size must be between 1 and FASTIX_PAGE_SIZE. */
void slab_init(struct slab_cache *c, const char *name, size_t obj_size,
               struct slab_provider provider);

/* Returns a zeroed object, or NULL when the provider has no more pages. */
LIBOS_MUST_CHECK void *slab_alloc(struct slab_cache *c);

/* Returns an object to its cache. */
void slab_free(struct slab_cache *c, void *obj);

#endif /* LIBOS_SLAB_H */
