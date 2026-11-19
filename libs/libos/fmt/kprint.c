/* kprint.c - minimal debug console output (see libos/kprint.h). */
#include <libos/kprint.h>
#include <sel4/sel4.h>

static void kputc(char c)
{
#ifdef CONFIG_PRINTING
    seL4_DebugPutChar(c);
#else
    (void)c;
#endif
}

void kprint_str(const char *s)
{
    while (*s != '\0') {
        kputc(*s++);
    }
}

void kprint_u64(uint64_t v)
{
    char buf[20]; /* UINT64_MAX has 20 decimal digits */
    int n = 0;

    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        kputc(buf[--n]);
    }
}

void kprint_hex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";

    kprint_str("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        kputc(digits[(v >> shift) & 0xF]);
    }
}
