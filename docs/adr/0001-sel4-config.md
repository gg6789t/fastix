# ADR 0001: seL4 version, kernel configuration, and verification scope

**Status:** Accepted (Phase 0, 2026-11-19)

## Decision
- **seL4 17.0.0** (tag `17.0.0`, commit `62e345c240a15671484530b4e6e069ce266c28d7`), unmodified.
- `seL4_tools` at `6493e0c6fab360eb81a8115a49c9ca7bc280c48a` (elfloader + cmake helpers; no release tags).
- `util_libs` at `ef49a397b7044fc38b609a99a8c6265a97913b8e` (only `libcpio`, needed by the elfloader).
- Development config (`config/aarch64-qemu-virt.cmake`): `qemu-arm-virt`, AArch64, Cortex-A53, GICv2,
  1024 MiB, `KernelIsMCS=ON`, `KernelFastpath=ON`, `KernelMaxNumNodes=1`, `KernelDebugBuild=ON`,
  `KernelPrinting=ON`, hypervisor extensions OFF, `LibSel4UseThreadLocals=OFF`.

## Verification scope (honest statement)
Source: `third_party/seL4/CAVEATS.md` at the pinned tag.
- seL4's AArch64 proofs cover **Armv8-A with hypervisor extensions only**, on a listed set of boards
  (tx2, bcm2711, bcm2712, imx8mm, imx8mq, imx93, maaxboard, odroidc2, odroidc4, rockpro64).
  `qemu-arm-virt` is not in that list, and our config has hypervisor extensions off.
- **MCS on AArch64:** "Functional correctness proofs for MCS on AArch64 are in progress" (CAVEATS.md).
- **SMP + MCS** is supported but described as less tested / experimental.
- Debug builds (`KernelDebugBuild=ON`, `KernelPrinting=ON`) are never the verified configuration.

**Therefore:** Fastix's current configuration is **built on seL4**, not a formally verified kernel
configuration. Documentation must not claim otherwise. A future release config that targets a proven
configuration would need: a listed board, hypervisor extensions on, release (non-debug) build, and
either non-MCS or a completed AArch64 MCS proof.

## Other Phase 0 build decisions
- **Toolchain:** Homebrew LLVM clang 23 + `ld.lld`, target triple `aarch64-none-elf`, linker forced
  with `-fuse-ld=lld` (otherwise clang calls Apple's Mach-O `ld`).
- **`-std=gnu17` for code that includes seL4 headers:** libsel4 uses the GNU `asm` keyword, which
  `-std=c17` rejects. All other strict warning flags apply. Userland stays `-std=c17` (AGENTS.md rule 3).
- **Paths with spaces:** seL4's build pastes paths into flags unquoted, so `tools/build.sh` builds
  through a space-free symlink (`~/.cache/fastix-src-<hash>`) when the checkout path has spaces.
- **GNU cpio** (Homebrew `cpio`, keg-only) is required by seL4's `MakeCPIO`; macOS `bsdcpio` lacks the flags.
- **Empty `gcc` link target:** the elfloader links `gcc` (libgcc) by name; nothing in it needs libgcc
  on AArch64 with clang, so an empty CMake interface target satisfies the name.
- **Thread-local IPC buffer pointer off:** servers are single-threaded event loops (AGENTS.md 5.5).
  Userland (Phase 5) will need its own per-thread handling in `libfastix_sys`.
- **Test exit:** tests stop QEMU with Arm semihosting `SYS_EXIT` from user mode
  (`-semihosting-config enable=on,userspace=on`, QEMU only).
