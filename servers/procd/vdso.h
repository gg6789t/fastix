/*
 * vdso.h - the global VDSO clock page and per-process info pages
 * (layouts in fastix/vdso.h). procd keeps the clock page mapped writable
 * for itself; processes see it read-only at FASTIX_VDSO_ADDR.
 */
#ifndef PROCD_VDSO_H
#define PROCD_VDSO_H

#include <stdint.h>

#include <fastix/vdso.h>
#include <libos/compiler.h>

#include "frame.h"

LIBOS_MUST_CHECK int vdso_init(void);

/* The shared clock page (map it read-only into each process). */
struct frame *vdso_frame(void);

/* CLOCK_MONOTONIC now, from the same data processes read. */
uint64_t vdso_mono_ns(void);

/* CLOCK_REALTIME - CLOCK_MONOTONIC (0 until an RTC driver sets the wall clock). */
uint64_t vdso_realtime_offset_ns(void);

#endif /* PROCD_VDSO_H */
