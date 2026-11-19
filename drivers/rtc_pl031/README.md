# rtc_pl031

Isolated driver for the ARM PL031 real-time clock. It owns only the PL031 MMIO frame.

- **At start:** reads the wall-clock time and sends it to `procd` once
  (`FASTIX_SRV_SET_REALTIME_NS`), so `CLOCK_REALTIME` in the VDSO shows the real date.
  This is the one approved exception to "drivers call procd only for memory/caps" (AGENTS.md 14.5 note).
- **Operations:** `RTC_OP_READ` returns the current Unix time in seconds.
- **State:** none; single-threaded loop; IPC rank 20.
