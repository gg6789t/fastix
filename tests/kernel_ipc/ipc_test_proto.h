/*
 * ipc_test_proto.h - protocol between procd's Phase 1 self-test and the test
 * child (tests/kernel_ipc/child.c). Test-only labels (abi.h test range).
 */
#ifndef IPC_TEST_PROTO_H
#define IPC_TEST_PROTO_H

#include <fastix/abi.h>

/* PING: MR0 = n. procd replies MR0 = n + 1. */
#define TEST_OP_PING      (FASTIX_IPC_LABEL_TEST_BASE + 0)
/* DONE: MR0 = wrong replies seen, MR1 = counter ticks for all PINGs. Empty reply. */
#define TEST_OP_DONE      (FASTIX_IPC_LABEL_TEST_BASE + 1)

/* After DONE the child reads this address (inside the null guard) to fault on purpose. */
#define TEST_FAULT_ADDR   0x8

#define TEST_CYCLES       100     /* create -> IPC -> fault -> destroy cycles */
#define TEST_PINGS_FIRST  100000  /* PINGs in the first cycle */
#define TEST_PINGS_OTHER  1000    /* PINGs in every later cycle */

#endif /* IPC_TEST_PROTO_H */
