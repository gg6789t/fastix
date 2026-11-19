# Fastix Progress

## Current Phase
**Phase 1: libos, Untyped/Cap Allocator, IPC & Multi-Task Bootstrap.** All exit criteria met (2026-11-19). Next: Phase 2.

## Phase 1 Checklist
- [x] Host tests under ASan+UBSan, 100+ random seeds (`tools/test_host.sh`, 3/3 pass):
  - buddy untyped allocator: 200 seeds × 3,000 ops against a model of seL4 untyped caps, with injected failures. It catches 3 planted bugs.
  - slot allocator: 200 seeds × 5,000 ops.
  - ELF reader: bad-header cases plus 200 seeds × 2,000 random corruptions.
- [x] QEMU: an unprivileged child (whose only cap is its badged endpoint) makes 100,000 IPC calls to procd, procd verifies every reply, and procd catches the child's deliberate fault (VM fault at 0x8)
- [x] QEMU: 100 create/destroy cycles (199,000 calls in total) with **0 leaks**. Free untyped bytes (1,070,921,824), tracked blocks (40) and used slots (11) are identical before and after; a planted slot leak is caught on cycle 0
- [x] Zero warnings in Fastix code (`-Werror`); `tools/build.sh` from clean succeeds

## Done Log
- 2026-11-19: **Phase 1 done.** procd now owns all RAM and can build, run and fully destroy isolated processes. Verified with `tools/test_host.sh` (3/3 pass) and `python3 tests/run_qemu_tests.py` (4/4 pass, QEMU exit 0).
- 2026-11-19: Moved slab/interval tree/frame array → Phase 2, SPSC ring → Phase 3, hash/radix → Phase 4, locks → Phase 9, warm pools/SIMD → when benchmarks show the need (AGENTS.md rule 2.1.6). Plan updated.
- 2026-11-19: **Phase 0 done** (commit `54fe7c5`). seL4 17.0.0 boots procd in QEMU; headless runner exits 0.

## Next Steps
1. Phase 2: slab allocator + interval tree (host-tested), then the VMA tree in procd.
2. Phase 2: demand-zero paging, stack growth, copy-on-write, `mmap`/`munmap`/`mprotect`/`brk`.
3. Phase 2: full ELF loader (System V initial stack, auxv), VDSO page + per-process info page.

## Known Issues & Open Questions
- seL4's build can't handle spaces in paths; `tools/build.sh` builds through `~/.cache/fastix-src-<hash>`. Renaming `my stuff` would remove that.
- The kernel config is not a formally verified seL4 configuration (ADR 0001).
- The Phase 1 ELF loader rejects programs whose segments share a page (lld output with `-z max-page-size=4096` is fine). Phase 2 handles this.
- Each kernel object uses its own untyped block (one retype + one revoke per object). This is simple and leak-proof, but slower than warm object pools; to be measured with `bench_fork_exit` in Phase 3.
- IPC round trip measured about 28 µs in QEMU TCG with a debug kernel. That is emulation, **not** a benchmark.
- Upstream seL4 build warnings remain (1 `ld.lld` `.rodata` alignment, 2 clang "unused `-c`").

## Key Numbers (QEMU TCG on Apple Silicon, debug kernel, Fastix code at -O2)
| Item | Measured | Budget (AGENTS.md 1.4 / 14.4) |
| :--- | :--- | :--- |
| seL4 kernel `.text` | 226,100 B | <= 250 KB |
| `procd` text+rodata (incl. embedded test child) | 21,452 B | <= 192 KB |
| `procd` bss (allocator tables + 32 KB stack) | 166,280 B | counts toward idle RAM <= 16 MB |
| Full boot image | 355,304 B | <= 4 MB |
| QEMU start → all Phase 1 tests done | 5.8 s | (test time, not boot time) |
