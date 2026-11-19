/*
 * servers/devd/devd_internal.h - internal structures and helpers shared across devd.
 */
#ifndef FASTIX_DEVD_INTERNAL_H
#define FASTIX_DEVD_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <fastix/dev_proto.h>
#include <libos/compiler.h>
#include <libos/ut_buddy.h>
#include <sel4/sel4.h>

#define DEVD_MAX_DRV_PTS    16
#define DEVD_MAX_DRV_FRAMES 48

struct dkobj {
    struct ut_block *block;
    seL4_CPtr cap;
};

struct drv_image {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
};

/* Static hardware description for one driver, discovered from the DTB. */
struct drv_spec {
    uint32_t kind;
    bool present;
    bool has_mmio;
    bool has_irq;
    bool irq_edge;
    bool bind_irq_ntfn;
    bool grant_rtc_procd;
    uint64_t mmio_paddr;
    uint32_t mmio_offset;
    uint32_t mmio_size;
    uint32_t irq_num;
    uint32_t dma_count;
};

/* Live runtime state of one isolated driver instance. */
struct drv_inst {
    struct drv_spec spec;
    bool running;
    uint32_t restarts;

    /* Persistent across restarts so clients never lose their endpoint cap. */
    struct dkobj service_ep;
    seL4_CPtr mmio_master_cap; /* owned by kobj.c mmio cache, never freed */
    seL4_CPtr irq_master_cap;  /* claimed once from IRQControl, reused on restart */

    /* Per-generation objects destroyed and recreated on every restart. */
    struct dkobj vspace;
    struct dkobj cnode;
    struct dkobj tcb;
    struct dkobj sc;
    struct dkobj reply;
    struct dkobj irq_ntfn;
    struct dkobj ipcbuf;
    struct dkobj bootinfo;
    struct dkobj dma[2];
    seL4_CPtr fault_ep_badged;

    struct dkobj pts[DEVD_MAX_DRV_PTS];
    size_t n_pts;
    struct dkobj frames[DEVD_MAX_DRV_FRAMES];
    size_t n_frames;
};

/* kobj.c */
LIBOS_MUST_CHECK int devd_kobj_init(const struct devd_bootinfo *bi);
LIBOS_MUST_CHECK int dkobj_alloc(seL4_Word type, seL4_Word param, struct dkobj *out);
void dkobj_free(struct dkobj *obj);
LIBOS_MUST_CHECK int dslot_get(seL4_CPtr *slot);
void dslot_put(seL4_CPtr slot);
LIBOS_MUST_CHECK int dslot_delete_and_put(seL4_CPtr slot);
LIBOS_MUST_CHECK int devd_mmio_frame(uint64_t page_paddr, seL4_CPtr *cap_out);
LIBOS_MUST_CHECK int devd_map_scratch(seL4_CPtr frame, bool is_mmio);
void devd_unmap_scratch(seL4_CPtr frame);
void devd_kobj_stats(uint64_t *free_bytes, uint64_t *slots_used);

/* images.c */
const struct drv_image *drv_image_get(uint32_t kind);

/* spawn.c */
LIBOS_MUST_CHECK int drv_start_first(struct drv_inst *d);
LIBOS_MUST_CHECK int drv_restart(struct drv_inst *d);
void drv_unblock_caller_eio(struct drv_inst *d);

/* main.c */
void devd_entry(const struct devd_bootinfo *bi);
_Noreturn void devd_main(const struct devd_bootinfo *bi);

#endif /* FASTIX_DEVD_INTERNAL_H */
