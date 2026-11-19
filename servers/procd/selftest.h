/* selftest.h - procd's Phase 1 boot-time self-test (IPC, fault handling, zero leaks). */
#ifndef PROCD_SELFTEST_H
#define PROCD_SELFTEST_H

#include <sel4/sel4.h>

/* Runs the test, printing [FASTIX-TEST ...] lines. Returns the number of failures. */
int selftest_run(const seL4_BootInfo *bi);

#endif /* PROCD_SELFTEST_H */
