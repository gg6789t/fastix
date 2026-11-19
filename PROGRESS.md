# Fastix Progress

## Current Phase
**Phase 2: Virtual Memory, ELF64 Loader & VDSO.** All exit criteria met (2026-11-19). Next: Phase 3.

## Phase 2 Checklist
- [x] Host tests, ASan+UBSan, 200 seeds each (`tools/test_host.sh`, 6/6 pass): slab, range tree (AVL), VDSO clock math, plus the earlier buddy, slot and ELF tests (ELF now covers PIE). Planted bugs in slab/rtree (5) are all caught.
- [x] NULL dereference → SIGSEGV (MAPERR at 0x8)
- [x] Stack overflow into the guard gap → SIGSEGV (MAPERR just below the 8 MB stack)
- [x] `mprotect(PROT_READ)` then write → SIGSEGV (ACCERR)
- [x] Partial `munmap` splits a VMA (1 → 2 VMAs; remaining pages keep their data; the hole faults)
- [x] Copy-on-write: the child's write stays private; afterwards the sole-owner write does not copy (frame count unchanged)
- [x] VDSO: `getpid` + `clock_gettime` agree with the IPC versions, are monotonic, and 10,000 reads send **0** messages
- [x] 500 × `mmap`+touch+`munmap` and 100 × `fork`+exit/fault+`wait`: **0 leaks** (free bytes, slots, frames, pages, VMAs, page tables, processes all identical)
- [x] Static ELF and static PIE both load with a correct System V stack and auxv (child checks 11 things, exits 42)
- [x] Phase 1 checks still covered: 100,000 IPC calls, controlled faults, zero-leak create/destroy
- [x] 14/14 QEMU tests pass, QEMU exit 0; 3 planted procd bugs (COW, zeroing, leak) are all caught

## Done Log
- 2026-11-19: Published at https://github.com/gg6789t/fastix (public); repo info added to AGENTS.md 4.3.
- 2026-11-19: **Phase 2 done.** procd is now a real server: virtual memory, copy-on-write `fork`, `wait`, a program loader, and VDSO fast paths. Verified with a clean `tools/build.sh`, `tools/test_host.sh` (6/6) and `tests/run_qemu_tests.py` (14/14, exit 0).
- 2026-11-19: Phase 1 done (`b7cd478`): buddy allocator, process spawn/destroy, zero-leak test.
- 2026-11-19: Phase 0 done (`54fe7c5`): seL4 17.0.0 boots procd in QEMU.

## Next Steps
1. Phase 3: full process lifecycle (`execve`, `vfork`/`posix_spawn`, process groups/sessions, credentials), `MAP_SHARED` anonymous memory.
2. Phase 3: signals (`sigaction`, delivery on the user stack, `sigreturn` via a VDSO trampoline, `EINTR`/`SA_RESTART`).
3. Phase 3: threads, futex, timers, SPSC pipe rings.

## Known Issues & Open Questions
- seL4's build can't handle spaces in paths (`tools/build.sh` symlink workaround). Renaming `my stuff` would remove it.
- Kernel config is not a formally verified seL4 configuration (ADR 0001).
- Frames and heap pages are pooled and never returned to the untyped allocator (no memory-pressure reclaim yet).
- Page tables stay with an address space until it exits (they are not freed on `munmap`).
- `AT_RANDOM` bytes are derived from the clock counter (not cryptographic) until `virtio-rng` (Phase 4). `CLOCK_REALTIME` starts at 0 until an RTC driver (Phase 4).
- A parent parked in `wait4` whose matching children all disappear without exiting would wait forever (cannot happen yet; to be handled with signals in Phase 3).
- `procd` scans its whole 4096-entry process table on exit/wait. That's fine now, but should use child lists later.
- The test programs need seL4 (`tests/vm`); real userland arrives with musl in Phase 5.

## Key Numbers (QEMU TCG on Apple Silicon, debug kernel, Fastix code -O2)
| Item | Measured | Budget |
| :--- | :--- | :--- |
| seL4 kernel `.text` | 226,100 B | <= 250 KB |
| `procd` `.text` (own code) | 25,148 B | <= 192 KB (text+rodata) |
| `procd` `.rodata` (incl. ~14.5 KB embedded test programs) | 17,112 B | (included above) |
| Full boot image | 372,200 B | <= 4 MB |
| All Phase 2 QEMU tests, start to finish | ~4.4 s | test time, not boot time |
