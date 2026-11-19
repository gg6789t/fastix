/*
 * selftest.c - Phase 1 self-test. Spawns the embedded test child
 * TEST_CYCLES times. Each time procd answers its PING calls, receives its
 * deliberate fault, destroys it, and checks that free memory, tracked blocks
 * and used CNode slots are exactly what they were before the first cycle.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <arch/counter.h>
#include <fastix/cspace.h>
#include <libos/kprint.h>

#include "ipc_test_proto.h"
#include "kobj.h"
#include "proc.h"
#include "selftest.h"

/* The test child ELF, embedded by the build (child_blob.S). */
extern const uint8_t fastix_test_child_start[];
extern const uint8_t fastix_test_child_end[];

#define TEST_PID 2 /* any non-init pid; the generation changes every cycle */

struct cycle_result {
    uint64_t pings;
    uint64_t wrong;
    uint64_t ticks;
    seL4_Word fault_label;
    uint64_t fault_addr;
};

/* Serves one child until it faults. Returns NULL, or what went wrong. */
static const char *serve_child(const struct proc *p, seL4_CPtr ep, seL4_CPtr reply,
                               struct cycle_result *r)
{
    seL4_Word badge;
    seL4_MessageInfo_t info = seL4_Recv(ep, &badge, reply);
    bool done = false;

    for (;;) {
        if (fastix_badge_pid(badge) != p->pid || fastix_badge_gen(badge) != p->gen) {
            return "message with a stale or unknown badge";
        }
        seL4_Word label = seL4_MessageInfo_get_label(info);
        if (fastix_badge_type(badge) == FASTIX_BADGE_FAULT) {
            r->fault_label = label;
            r->fault_addr = seL4_GetMR(seL4_VMFault_Addr);
            return done ? NULL : "fault before DONE";
        }
        if (fastix_badge_type(badge) != FASTIX_BADGE_SYSCALL) {
            return "unexpected badge type";
        }
        if (label == TEST_OP_PING) {
            uint64_t n = seL4_GetMR(0);
            if (n != r->pings) {
                return "PING out of order";
            }
            r->pings++;
            seL4_SetMR(0, n + 1);
            info = seL4_ReplyRecv(ep, seL4_MessageInfo_new(0, 0, 0, 1), &badge, reply);
        } else if (label == TEST_OP_DONE) {
            r->wrong = seL4_GetMR(0);
            r->ticks = seL4_GetMR(1);
            done = true;
            info = seL4_ReplyRecv(ep, seL4_MessageInfo_new(0, 0, 0, 0), &badge, reply);
        } else {
            return "unknown op";
        }
    }
}

static bool same_stats(struct kobj_stats a, struct kobj_stats b)
{
    return a.free_bytes == b.free_bytes && a.blocks_in_use == b.blocks_in_use &&
           a.slots_used == b.slots_used;
}

static void print_stats(const char *name, struct kobj_stats s)
{
    kprint_str("selftest: ");
    kprint_str(name);
    kprint_str(" free_bytes=");
    kprint_u64(s.free_bytes);
    kprint_str(" blocks=");
    kprint_u64(s.blocks_in_use);
    kprint_str(" slots=");
    kprint_u64(s.slots_used);
    kprint_str("\n");
}

/* One create -> serve -> fault -> destroy cycle, then the leak check. */
static const char *run_cycle(unsigned cycle, const seL4_BootInfo *bi, seL4_CPtr ep,
                             seL4_CPtr reply, struct kobj_stats base, struct cycle_result *r)
{
    uint64_t want = (cycle == 0) ? TEST_PINGS_FIRST : TEST_PINGS_OTHER;
    struct proc_spawn_args args = {
        .pid = TEST_PID,
        .gen = (uint16_t)(cycle + 1),
        .elf = fastix_test_child_start,
        .elf_size = (size_t)(fastix_test_child_end - fastix_test_child_start),
        .endpoint = ep,
        .sched_control = bi->schedcontrol.start,
        .arg0 = want,
    };
    struct proc p;

    if (proc_spawn(&p, &args) != 0) {
        return "proc_spawn failed";
    }
    const char *problem = serve_child(&p, ep, reply, r);
    proc_destroy(&p);

    if (problem != NULL) {
        return problem;
    }
    if (r->fault_label != seL4_Fault_VMFault || r->fault_addr != TEST_FAULT_ADDR) {
        return "wrong fault type or address";
    }
    if (r->pings != want || r->wrong != 0) {
        return "PING count or replies wrong";
    }
    if (!same_stats(kobj_stats(), base)) {
        return "resources leaked";
    }
    return NULL;
}

int selftest_run(const seL4_BootInfo *bi)
{
    struct kobj ep, reply;
    struct kobj_stats empty = kobj_stats();

    if (kobj_alloc(seL4_EndpointObject, 0, &ep) != 0) {
        kprint_str("[FASTIX-TEST FAIL] ipc_setup: cannot create endpoint\n");
        return 1;
    }
    if (kobj_alloc(seL4_ReplyObject, 0, &reply) != 0) {
        kobj_free(&ep);
        kprint_str("[FASTIX-TEST FAIL] ipc_setup: cannot create reply object\n");
        return 1;
    }
    struct kobj_stats base = kobj_stats();
    struct cycle_result first = { 0 };
    uint64_t total_pings = 0;
    unsigned cycle;
    const char *problem = NULL;

    print_stats("before", base);
    for (cycle = 0; cycle < TEST_CYCLES && problem == NULL; cycle++) {
        struct cycle_result r = { 0 };
        problem = run_cycle(cycle, bi, ep.cap, reply.cap, base, &r);
        total_pings += r.pings;
        if (cycle == 0) {
            first = r;
        }
    }
    print_stats("after ", kobj_stats());

    kobj_free(&reply);
    kobj_free(&ep);
    if (problem == NULL && !same_stats(kobj_stats(), empty)) {
        problem = "endpoint/reply not returned";
    }
    if (problem != NULL) {
        kprint_str("[FASTIX-TEST FAIL] ipc_fault_leak: cycle ");
        kprint_u64(cycle - 1);
        kprint_str(": ");
        kprint_str(problem);
        kprint_str("\n");
        return 1;
    }

    uint64_t ns = first.ticks * 1000000000ULL / arch_counter_freq();
    kprint_str("selftest: cycles=");
    kprint_u64(TEST_CYCLES);
    kprint_str(" total_pings=");
    kprint_u64(total_pings);
    kprint_str(" first_cycle_ns_per_call=");
    kprint_u64(ns / TEST_PINGS_FIRST);
    kprint_str(" (QEMU TCG, not a benchmark)\n");
    kprint_str("[FASTIX-TEST PASS] ipc_100k_calls\n");
    kprint_str("[FASTIX-TEST PASS] controlled_fault_caught\n");
    kprint_str("[FASTIX-TEST PASS] zero_leak_100_cycles\n");
    return 0;
}
