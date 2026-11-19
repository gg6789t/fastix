/*
 * main.c - procd entry. procd is the seL4 root task: it checks what seL4
 * handed it, sets up its allocators, starts PID 1 from the first embedded
 * image, and then serves syscalls and faults forever.
 */
#include <stddef.h>
#include <stdint.h>

#include <arch/counter.h>
#include <arch/qemu_exit.h>
#include <fastix/vspace.h>
#include <libos/kprint.h>
#include <sel4/sel4.h>

#include "cspace.h"
#include "frame.h"
#include "images.h"
#include "kobj.h"
#include "proc.h"
#include "procd.h"
#include "reply.h"
#include "server.h"
#include "vdso.h"
#include "vmspace.h"

_Static_assert(FASTIX_VSPACE_USER_END - 1 <= seL4_UserTop,
               "Fastix user layout does not fit the seL4 user address space");

struct untyped_totals {
    uint64_t ram_caps;
    uint64_t ram_bytes;
    uint64_t dev_caps;
    uint64_t dev_bytes;
};

static void print_kv(const char *key, uint64_t value)
{
    kprint_str("bootinfo: ");
    kprint_str(key);
    kprint_str("=");
    kprint_u64(value);
    kprint_str("\n");
}

static struct untyped_totals sum_untypeds(const seL4_BootInfo *bi)
{
    struct untyped_totals t = { 0, 0, 0, 0 };

    for (seL4_Word i = 0; i < bi->untyped.end - bi->untyped.start; i++) {
        const seL4_UntypedDesc *ut = &bi->untypedList[i];
        uint64_t bytes = (uint64_t)1 << ut->sizeBits;
        if (ut->isDevice) {
            t.dev_caps++;
            t.dev_bytes += bytes;
        } else {
            t.ram_caps++;
            t.ram_bytes += bytes;
        }
    }
    return t;
}

/* Returns NULL if the BootInfo looks sane, otherwise the reason it does not. */
static const char *check_bootinfo(const seL4_BootInfo *bi, const struct untyped_totals *t)
{
    seL4_Word untyped_count = bi->untyped.end - bi->untyped.start;

    if (bi->numNodes != 1) {
        return "expected exactly 1 CPU node in this configuration";
    }
    if (untyped_count == 0 || untyped_count > CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS) {
        return "untyped cap count out of range";
    }
    if (t->ram_caps == 0 || t->ram_bytes == 0 || t->dev_caps == 0) {
        return "no RAM or device untypeds";
    }
    if (bi->empty.end <= bi->empty.start || bi->initThreadCNodeSizeBits == 0 ||
        bi->ipcBuffer == NULL) {
        return "missing empty slots, root CNode size or IPC buffer";
    }
    return NULL;
}

static void print_bootinfo(const seL4_BootInfo *bi, const struct untyped_totals *t)
{
    print_kv("nodes", bi->numNodes);
    print_kv("cnode_size_bits", bi->initThreadCNodeSizeBits);
    print_kv("empty_slots", bi->empty.end - bi->empty.start);
    print_kv("untyped_caps", bi->untyped.end - bi->untyped.start);
    print_kv("ram_untyped_mib", t->ram_bytes >> 20);
    print_kv("device_untyped_caps", t->dev_caps);
}

/* Sets up every procd module. Returns NULL, or what failed. */
static const char *setup(const seL4_BootInfo *bi)
{
    seL4_Word cspace_untyped, num_slots;
    seL4_CPtr first_slot;

    if (cspace_init(bi, &cspace_untyped, &first_slot, &num_slots) != 0) {
        return "procd CSpace";
    }
    if (kobj_init(bi, cspace_untyped, first_slot, num_slots) != 0) {
        return "kernel object allocator";
    }
    if (frame_init() != 0) {
        return "frame scratch window";
    }
    vms_module_init();
    reply_module_init();
    if (vdso_init() != 0) {
        return "VDSO page";
    }
    if (server_init() != 0) {
        return "server endpoint";
    }
    proc_module_init(server_endpoint(), bi->schedcontrol.start);
    return NULL;
}

static const char *start_init(void)
{
    const struct image *img = image_get(0);
    const char *argv[] = { img->name, NULL };
    const char *envp[] = { "FASTIX=1", NULL };
    struct load_args la = { img->start, (size_t)(img->end - img->start), argv, envp,
                            arch_read_counter() };
    struct proc *init;

    return proc_spawn(&la, 0, &init) == 0 ? NULL : "spawning PID 1";
}

_Noreturn void procd_main(seL4_BootInfo *bi)
{
    /* libsel4 passes extra message words and caps through this buffer. */
    seL4_SetIPCBuffer(bi->ipcBuffer);
    kprint_str("fastix: procd started (seL4 root task)\n");

    struct untyped_totals t = sum_untypeds(bi);
    print_bootinfo(bi, &t);
    const char *problem = check_bootinfo(bi, &t);
    if (problem == NULL) {
        kprint_str("[FASTIX-TEST PASS] bootinfo_summary\n");
        problem = setup(bi);
    }
    if (problem == NULL) {
        problem = start_init();
    }
    if (problem == NULL) {
        server_run();
    }
    kprint_str("[FASTIX-TEST FAIL] procd_boot: ");
    kprint_str(problem);
    kprint_str("\n[FASTIX-SUITE DONE rc=1]\n");
    arch_qemu_exit(1);
    (void)seL4_TCB_Suspend(seL4_CapInitThreadTCB);
    for (;;) {
    }
}
