/*
 * main.c - procd entry. procd is the seL4 root task.
 * Phase 0: print a summary of the memory and capabilities seL4 handed us,
 * check it is sane, report a test result, and stop QEMU.
 */
#include <stddef.h>
#include <stdint.h>

#include <arch/qemu_exit.h>
#include <fastix/vspace.h>
#include <libos/kprint.h>
#include <sel4/sel4.h>

#include "procd.h"

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
    seL4_Word count = bi->untyped.end - bi->untyped.start;

    for (seL4_Word i = 0; i < count; i++) {
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
    if (t->ram_caps == 0 || t->ram_bytes == 0) {
        return "no RAM untypeds";
    }
    if (t->dev_caps == 0) {
        return "no device untypeds";
    }
    if (bi->empty.end <= bi->empty.start) {
        return "no empty CNode slots";
    }
    if (bi->initThreadCNodeSizeBits == 0 || bi->ipcBuffer == NULL) {
        return "missing root CNode size or IPC buffer";
    }
    return NULL;
}

_Noreturn void procd_main(seL4_BootInfo *bi)
{
    kprint_str("fastix: procd started (seL4 root task)\n");

    struct untyped_totals t = sum_untypeds(bi);

    print_kv("nodes", bi->numNodes);
    print_kv("cnode_size_bits", bi->initThreadCNodeSizeBits);
    print_kv("empty_slots", bi->empty.end - bi->empty.start);
    print_kv("untyped_caps", bi->untyped.end - bi->untyped.start);
    print_kv("ram_untyped_caps", t.ram_caps);
    print_kv("ram_untyped_bytes", t.ram_bytes);
    print_kv("ram_untyped_mib", t.ram_bytes >> 20);
    print_kv("device_untyped_caps", t.dev_caps);
    print_kv("device_untyped_bytes", t.dev_bytes);
    kprint_str("bootinfo: ipc_buffer=");
    kprint_hex((uint64_t)(uintptr_t)bi->ipcBuffer);
    kprint_str("\n");

    const char *problem = check_bootinfo(bi, &t);
    int rc = (problem == NULL) ? 0 : 1;

    if (problem == NULL) {
        kprint_str("[FASTIX-TEST PASS] bootinfo_summary\n");
    } else {
        kprint_str("[FASTIX-TEST FAIL] bootinfo_summary: ");
        kprint_str(problem);
        kprint_str("\n");
    }
    kprint_str(rc == 0 ? "[FASTIX-SUITE DONE rc=0]\n" : "[FASTIX-SUITE DONE rc=1]\n");

    arch_qemu_exit(rc);

    /* Only reached if semihosting is off: stop this thread for good. */
    (void)seL4_TCB_Suspend(seL4_CapInitThreadTCB);
    for (;;) {
    }
}
