/* reply.c - Reply object pool (see reply.h). Objects are kept for reuse, never freed. */
#include <fastix/errno.h>
#include <libos/slab.h>
#include <sel4/sel4.h>

#include "heap.h"
#include "reply.h"

static struct slab_cache reply_cache;
static struct list_node free_replies;

void reply_module_init(void)
{
    slab_init(&reply_cache, "reply", sizeof(struct reply_obj), heap_provider());
    list_init(&free_replies);
}

int reply_get(struct reply_obj **out)
{
    struct list_node *n = list_pop(&free_replies);

    if (n != NULL) {
        *out = LIST_ENTRY(n, struct reply_obj, link);
        return 0;
    }
    struct reply_obj *r = slab_alloc(&reply_cache);
    if (r == NULL) {
        return -FASTIX_ENOMEM;
    }
    int err = kobj_alloc(seL4_ReplyObject, 0, &r->obj);
    if (err != 0) {
        slab_free(&reply_cache, r);
        return err;
    }
    *out = r;
    return 0;
}

void reply_put(struct reply_obj *r)
{
    list_push(&free_replies, &r->link);
}

void reply_send(struct reply_obj *r, unsigned n, const uint64_t *mr)
{
    for (unsigned i = 0; i < n; i++) {
        seL4_SetMR((int)i, mr[i]);
    }
    seL4_Send(r->obj.cap, seL4_MessageInfo_new(0, 0, 0, n));
    reply_put(r);
}
