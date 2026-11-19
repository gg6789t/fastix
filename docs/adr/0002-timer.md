# ADR 0002: Timer Strategy on AArch64 `qemu-arm-virt`

- **Status:** Accepted
- **Date:** 2026-11-19
- **Context:** Phase 4B (`devd` and isolated drivers) asks whether a user-level hardware timer driver should replace `servers/procd/ticker.c` on `qemu-arm-virt`.

## Evidence from QEMU DTB and seL4 17.0.0

1. **Hardware available in QEMU `virt` (`build/qemu-arm-virt.dts`):**
   - Only one timer node exists: `/timer` (`compatible = "arm,armv8-timer", "arm,armv7-timer"`), with 4 ARM Generic Timer PPIs:
     - PPI 29 (`16 + 13`): Secure Physical Timer (unavailable under `virt,secure=off`)
     - PPI 30 (`16 + 14`): Non-Secure EL1 Physical Timer (`CNTP_*_EL0`)
     - PPI 27 (`16 + 11`): Virtual Timer (`CNTV_*_EL0`)
     - PPI 26 (`16 + 10`): Non-Secure EL2 Physical Timer (`CNTHP_*_EL2`)
   - There is no separate MMIO timer device (no ARM SP804, no `arm,armv7-timer-mem`).

2. **Timer owned by seL4 (`build/kernel/gen_headers/plat/machine/devices_gen.h:34-39`, `third_party/seL4/include/arch/arm/arch/64/mode/machine.h:30-35`):**
   - With `KernelArmHypervisorSupport=OFF`, seL4 uses the Virtual Timer (`cntv_cval_el0`, `cntv_ctl_el0`, `cntvct_el0`) and reserves `KERNEL_TIMER_IRQ = 27` (`setIRQState(IRQTimer, 27)` in `third_party/seL4/src/arch/arm/kernel/boot.c:126`) to drive MCS scheduling refills.
   - `KernelArmExportVCNTUser=ON` exposes only the read-only virtual counter (`cntvct_el0`) and frequency (`cntfrq_el0`) to EL0, which Fastix's VDSO uses for zero-IPC `clock_gettime`.

3. **Why exporting `CNTP_*` to userspace (`KernelArmExportPTMRUser=ON`) is unsafe:**
   - In `third_party/seL4/src/arch/arm/armv/armv8-a/64/user_access.c:37-52`, `CONFIG_EXPORT_PTMR_USER` sets `CNTKCTL_EL1.EL0PTEN` globally once at boot. seL4 does not save/restore `CNTKCTL_EL1` per TCB.
   - Therefore, enabling `KernelArmExportPTMRUser=ON` grants **every** untrusted EL0 user process direct write access to `CNTP_CTL_EL0` and `CNTP_CVAL_EL0`, letting any user process disable or forge the system timer for the whole OS.
   - seL4's own Kconfig (`third_party/seL4/src/drivers/timer/config.cmake:53-62`) explicitly warns:
     > *"WARNING: selecting this option opens a storage channel and allows threads to easily corrupt these registers for each other, this should only be used for debugging / development purposes"*

## Decision

1. **Keep `servers/procd/ticker.c` on `qemu-arm-virt`** and keep `KernelArmExportPTMRUser=OFF` and `KernelArmExportVTMRUser=OFF`.
   - `ticker.c` uses seL4 MCS's own kernel-managed hardware timer (`SchedContext` period = 1 ms, budget = 200 us) via a 10-line helper thread that stays suspended (`seL4_TCB_Suspend`) whenever no timer or sleeper is armed (`armed_count == 0`).
   - No untrusted EL0 process can access, mask, or perturb the timer hardware or `ticker.c`'s notification capability.
2. **Use an isolated `rtc_pl031` driver under `devd` for wall-clock time (`CLOCK_REALTIME`):**
   - PL031 is a real isolated MMIO device (`0x09010000`, SPI IRQ 34) with no shared EL0 system registers. `drivers/rtc_pl031` reads Unix epoch seconds and pushes the offset downward to `procd`'s VDSO.
3. **Future hardware / Tier 2 (`x86_64`):**
   - On platforms with a dedicated MMIO timer (e.g., x86_64 HPET/PIT or ARM boards with an MMIO timer like `bcm2835-system-timer`), `devd` will spawn an isolated `drivers/timer` mapped only to that MMIO frame and IRQ, signalling `procd`'s timer notification.

## Consequences

- Preserves strict process isolation on AArch64 `qemu-arm-virt` with zero unsafe kernel debug flags.
- Keeps `procd`'s timer path small, deterministic, and idle-free when no timers are armed.
- Timer granularity on `qemu-arm-virt` remains 1 ms (MCS tick period); monotonic clock reads remain nanosecond-resolution via `cntvct_el0` in VDSO.
