# fault_test

Test-only driver used to prove `devd`'s crash recovery. It owns no hardware.

- **Operations:** `FAULT_OP_PING` (returns `val + 1` and its restart generation) and
  `FAULT_OP_CRASH` (faults on purpose in the middle of a call; the caller gets `-EIO`).
- **Test:** `devd_driver_fault_restart_zero_leak` crashes it 3 times and checks it is restarted on
  the same endpoint with zero leaked memory or slots, while other drivers keep working.
