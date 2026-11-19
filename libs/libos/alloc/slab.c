/* slab.c - fixed-size object allocator (see libos/slab.h). */
#include <libos/assert.h>
#include <libos/mem.h>
#include <libos/slab.h>

#include <fastix/config.h>

struct slab_free {
    struct slab_free *next;
};

void slab_init(struct slab_cache *c, const char *name, size_t obj_size,
               struct slab_provider provider)
{
    libos_assert(obj_size > 0 && obj_size <= FASTIX_PAGE_SIZE);
    c->name = name;
    c->obj_size = (obj_size + SLAB_ALIGN - 1) & ~(size_t)(SLAB_ALIGN - 1);
    c->free = NULL;
    c->in_use = 0;
    c->pages = 0;
    c->provider = provider;
}

/* Takes one page from the provider and puts all of its objects on the free list. */
static int grow(struct slab_cache *c)
{
    void *page;
    int err = c->provider.get_page(c->provider.ctx, &page);

    if (err != 0) {
        return err;
    }
    uint8_t *base = page;
    size_t count = FASTIX_PAGE_SIZE / c->obj_size;
    for (size_t i = count; i > 0; i--) {
        struct slab_free *f = (struct slab_free *)(void *)(base + (i - 1) * c->obj_size);
        f->next = c->free;
        c->free = f;
    }
    c->pages++;
    return 0;
}

void *slab_alloc(struct slab_cache *c)
{
    if (c->free == NULL && grow(c) != 0) {
        return NULL;
    }
    struct slab_free *f = c->free;
    c->free = f->next;
    c->in_use++;
    memset(f, 0, c->obj_size);
    return f;
}

void slab_free(struct slab_cache *c, void *obj)
{
    libos_assert(obj != NULL && c->in_use > 0);
    struct slab_free *f = obj;
    f->next = c->free;
    c->free = f;
    c->in_use--;
}
