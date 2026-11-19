# procd

The seL4 root task and process executive (AGENTS.md 6.3 B).

**Phase 0 (current):** starts from `arch/aarch64/crt0.S` with the seL4 BootInfo pointer, prints a
summary of the RAM/device memory and capability slots seL4 handed over, checks it is sane, prints a
`[FASTIX-TEST ...]` result, and stops QEMU.

- **IPC operations:** none yet.
- **Data structures:** none yet.
- **Locks / rank:** single-threaded; IPC rank 10 (AGENTS.md 14.5).
- **Files:** `main.c` (entry + BootInfo summary), `procd.h` (entry prototype).
