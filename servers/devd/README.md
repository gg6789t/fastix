# devd — Device Manager & Isolated Driver Supervisor

`devd` (Rank 20) is a single-threaded seL4 server spawned by `procd` at boot. It owns the hardware DTB, all Device Untypeds, and `IRQControl`, and launches each hardware driver in its own isolated protection domain with least-privilege capabilities.

## Responsibilities
- Parse the runtime Flattened Device Tree (FDT) supplied by seL4 (`SEL4_BOOTINFO_HEADER_FDT`) via `libdriver/dtb`.
- Carve exact 4 KB MMIO device frames by binary-splitting Device Untypeds (`kobj.c`). Kernel-object allocation itself is the shared `libs/libos/alloc/kalloc.c` (same code as procd).
- Probe QEMU `virt` `virtio,mmio` slots to locate VirtIO-RNG (`DeviceID=4`) and VirtIO-BLK (`DeviceID=2`). Note: QEMU `virt` packs eight 512-byte VirtIO-MMIO slots per 4 KB page (`0x0a003000..0x0a003fff`), so VirtIO drivers on the same 4 KB hardware page receive a copy of that 4 KB MMIO frame cap with their exact slot offset while remaining isolated from PL011, PL031, GIC, and all other drivers' DMA/RAM.
- Launch isolated drivers (`uart_pl011`, `rtc_pl031`, `virtio_rng`, `virtio_blk`, `fault_test`), each with its own VSpace, CSpace, TCB, SchedContext, IPC buffer, stack, and only its own MMIO/DMA/IRQ capabilities.
- Register each driver's persistent `service_ep` downward with `procd` (Rank 20 → Rank 10). `devd` never makes synchronous `seL4_Call`s to any driver.
- Supervise driver fault endpoints (`DEVD_BADGE_FAULT | drv_kind`): on a driver fault, unblock any parked caller on the driver's `Reply` object with `-FASTIX_EIO`, tear down all per-generation objects with zero leaks, and restart the driver bound to the same `service_ep`.

## IPC Operations (`include/fastix/dev_proto.h`)
- `DEVD_OP_INFO`: returns active driver count, `devd` free RAM bytes, used CSpace slots, and DTB size.
- `DEVD_OP_DRV_STATUS(drv_kind)`: returns running state, restart count, MMIO physical address, and GIC IRQ number.

## Main Data Structures & Locks
- `dev_pool[]` + `mmio_cache[]` (`kobj.c`): Device Untyped binary-split pool and carved 4 KB MMIO frame cache.
- `buddy` + `slots` (`kobj.c`): `libos` `ut_buddy` (4 MB RAM untyped) and `slot_alloc` (4032 slots).
- `drivers[FASTIX_DRV_COUNT]` (`main.c`, `spawn.c`): static DTB specs and live per-driver kernel objects.
- **Locks:** None — `devd` is strictly single-threaded.
