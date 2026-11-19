/*
 * libos/mem.h - memcpy/memmove/memset/memcmp for code without a libc.
 * Hosted unit tests use the host libc instead. Simple byte loops for now;
 * faster versions only when a benchmark shows the need (AGENTS.md 2.1).
 */
#ifndef LIBOS_MEM_H
#define LIBOS_MEM_H

#ifdef LIBOS_HOSTED
#include <string.h>
#else
#include <stddef.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
#endif

#endif /* LIBOS_MEM_H */
