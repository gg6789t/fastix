# uart_pl011

Isolated driver for the ARM PL011 serial port (QEMU `virt` console). It owns only the PL011 MMIO
frame and its interrupt.

- **Operations** (`include/fastix/dev_proto.h`): `UART_OP_WRITE` (up to 48 bytes in message
  registers), `UART_OP_READ` (up to 48 bytes), `UART_OP_INFO` (MMIO address, IRQ, byte/IRQ counters).
- **State:** transmit and receive counters only; single-threaded loop; IPC rank 20.
- **Limits:** receive interrupts are bound but not yet tested with live input (the QEMU test runner
  has no stdin). `ttyd` (Phase 6) will be its real client.
