# Fastix Progress

## Current Phase
**Phase 0: Toolchain, Build System & seL4 Hello World.** All exit criteria met (2026-11-19). Next: Phase 1.

## Phase 0 Checklist
- [x] Host toolchain complete: `ld.lld` 23.1.6, GNU cpio 2.15, Python venv from `tools/requirements.txt` (fresh venv + clean build succeeded)
- [x] Repo skeleton + pinned `third_party/seL4` (17.0.0), `seL4_tools`, `util_libs` (git submodules; hashes in `docs/adr/0001-sel4-config.md`)
- [x] Single-source-of-truth headers: `include/fastix/{config,abi,cspace,vspace,errno}.h`. Each compiles alone under strict `-std=c17`; errno values diffed identical to musl (131 entries)
- [x] Single-command build (`tools/build.sh`) produces `build/fastix-image.elf`: 0 warnings in Fastix code (3 warnings, all from seL4's own build steps)
- [x] Root task (`procd`) prints the BootInfo summary: 1 node, 56 untyped caps, 34 RAM (1021 MiB), 22 device, CNode 2^12 slots, 3976 empty
- [x] `tests/run_qemu_tests.py` boots headless QEMU and exits 0; it also correctly fails on 5 fake failure cases (FAIL marker, no DONE, bad exit code, hang, no self-exit)

## Done Log
- 2026-11-19: **Phase 0 done.** seL4 17.0.0 (MCS, AArch64, QEMU virt) boots our root task, which prints what memory seL4 gave it, checks it, and shuts QEMU down with exit code 0. Verified with `tools/build.sh` (clean, fresh venv) and `python3 tests/run_qemu_tests.py` → `RESULT: PASS`.
- 2026-11-19: Started Phase 0. Installed `lld` and GNU `cpio`.

## Next Steps
1. Phase 1: `libos` basics with host unit tests (slab allocator, untyped/buddy allocator, list/hash/interval tree, SPSC ring), run with ASan+UBSan and 100+ random seeds.
2. Phase 1: `procd` untyped allocator + CSpace slot allocator using the real BootInfo.
3. Phase 1: spawn a first child process with a badged endpoint; 100,000 IPC calls, a fault, and 0 leaks over 100 create/destroy cycles.

## Known Issues & Open Questions
- seL4's build cannot handle spaces in paths. `tools/build.sh` works around it with a symlink at `~/.cache/fastix-src-<hash>`. Renaming the `my stuff` folder would remove the need.
- Current kernel config is **not** a formally verified seL4 configuration (debug build, QEMU board, MCS on AArch64 proofs still in progress). See ADR 0001.
- Upstream warnings remain in seL4's build: one `ld.lld` `.rodata` alignment warning on `kernel.elf`, and two clang "unused `-c`" warnings in libsel4 header generation.
- Changes are not committed to git yet (git repo initialized with submodules).

## Key Numbers (QEMU TCG on Apple Silicon, debug build)
| Item | Measured | Budget (AGENTS.md 1.4) |
| :--- | :--- | :--- |
| seL4 kernel `.text` | 226,100 B | <= 250 KB |
| `procd` `.text` | 4,474 B | <= 192 KB |
| Full boot image (`fastix-image.elf`) | 322,024 B | <= 4 MB |
| Host time from QEMU start to test DONE | 0.04–0.05 s | boot-to-shell <= 1.5 s (TCG) |
