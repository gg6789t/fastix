/*
 * libos/kprint.h - minimal debug console output for servers.
 * Writes through the seL4 debug console (debug kernels only); a no-op otherwise.
 */
#ifndef LIBOS_KPRINT_H
#define LIBOS_KPRINT_H

#include <stdint.h>

void kprint_str(const char *s);
void kprint_u64(uint64_t v);   /* decimal */
void kprint_hex(uint64_t v);   /* 0x-prefixed hex */

#endif /* LIBOS_KPRINT_H */
