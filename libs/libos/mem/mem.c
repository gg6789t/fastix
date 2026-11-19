/*
 * mem.c - memory primitives for libc-free code (see libos/mem.h).
 * Compiled with -ffreestanding (implies -fno-builtin), so the compiler will
 * not turn these loops back into calls to themselves.
 */
#include <libos/mem.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n-- > 0) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d < s) {
        while (n-- > 0) {
            *d++ = *s++;
        }
    } else {
        while (n-- > 0) {
            d[n] = s[n];
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    while (n-- > 0) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a;
    const unsigned char *y = b;

    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}
