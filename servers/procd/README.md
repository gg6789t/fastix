# procd

The seL4 root task and process executive (AGENTS.md 6.3 B).

**Current (Phase 1):** at boot procd prints and checks the seL4 BootInfo, takes over all RAM untypeds
and free CNode slots, then runs a self-test. The test spawns an unprivileged child process 100 times;
each time it serves the child's IPC calls, catches its deliberate page fault, destroys it, and checks
that nothing leaked.

- **IPC operations:** test-only `TEST_OP_PING` / `TEST_OP_DONE` (`tests/kernel_ipc/ipc_test_proto.h`).
  Real syscalls arrive from Phase 2 on.
- **Data structures:**
  - `kobj.c`: one untyped block (from `libos` `ut_buddy`) per kernel object. Freeing revokes the
    block, so the kernel deletes the object and every cap derived from it.
  - `vm.c`: `struct objlist` (every object a process owns), page mapping with on-demand page tables,
    and a scratch window for filling frames.
  - `proc.c`: `struct proc` plus spawn/destroy. A process gets a VSpace, a 64-slot CNode holding only
    its badged procd endpoint (slot 1), a TCB, a SchedContext, and a badged fault endpoint.
- **Locks / rank:** single-threaded; IPC rank 10 (AGENTS.md 14.5).
- **Files:** `main.c` (boot), `kobj.c`, `vm.c`, `proc.c`, `selftest.c` (Phase 1 test driver).
