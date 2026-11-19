/*
 * libos/assert.h - assertions for libos code.
 * Hosted unit tests (LIBOS_HOSTED) use the host's assert(); on seL4 a failed
 * assertion calls __assert_fail (libs/libos/assert.c).
 */
#ifndef LIBOS_ASSERT_H
#define LIBOS_ASSERT_H

#ifdef LIBOS_HOSTED
#include <assert.h>
#define libos_assert(expr) assert(expr)
#else
void __assert_fail(const char *str, const char *file, int line, const char *function);
#define libos_assert(expr) \
    do { \
        if (!(expr)) { \
            __assert_fail(#expr, __FILE__, __LINE__, __func__); \
        } \
    } while (0)
#endif

#endif /* LIBOS_ASSERT_H */
