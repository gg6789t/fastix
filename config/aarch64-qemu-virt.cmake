# aarch64-qemu-virt.cmake - seL4 + QEMU settings for the Tier 1 target.
# Included by the top-level CMakeLists.txt before project().
# See docs/adr/0001-sel4-config.md for why each kernel option is chosen.

set(KernelPlatform "qemu-arm-virt" CACHE STRING "" FORCE)
set(KernelSel4Arch "aarch64" CACHE STRING "" FORCE)
set(ARM_CPU "cortex-a53" CACHE STRING "" FORCE)
set(QEMU_MEMORY "1024" CACHE STRING "" FORCE)  # MiB; the runner reads it back from the build

# Kernel features (AGENTS.md 5.1)
set(KernelIsMCS ON CACHE BOOL "" FORCE)
set(KernelFastpath ON CACHE BOOL "" FORCE)
set(KernelArmExportVCNTUser ON CACHE BOOL "" FORCE)  # user-mode clock reads (VDSO, benchmarks)
set(KernelMaxNumNodes 1 CACHE STRING "" FORCE)   # SMP comes in Phase 9

# Phase 0 is a debug build: seL4 debug console on, verification build off.
set(KernelDebugBuild ON CACHE BOOL "" FORCE)
set(KernelPrinting ON CACHE BOOL "" FORCE)
set(KernelVerificationBuild OFF CACHE BOOL "" FORCE)

# Servers are single-threaded event loops, so the IPC buffer pointer
# does not need to be thread-local. Keeps startup code trivial.
set(LibSel4UseThreadLocals OFF CACHE BOOL "" FORCE)
