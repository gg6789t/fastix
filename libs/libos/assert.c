/*
 * assert.c - assertion failure handler for servers (no libc).
 * libsel4's seL4_Assert/seL4_Fail call this. It prints where it failed, then
 * halts the system in debug builds (so the bug is seen at once) or spins.
 */
#include <libos/kprint.h>
#include <sel4/sel4.h>

void __assert_fail(const char *str, const char *file, int line, const char *function)
{
    kprint_str("ASSERT FAILED: ");
    kprint_str(str);
    kprint_str(" at ");
    kprint_str(file);
    kprint_str(":");
    kprint_u64((uint64_t)line);
    kprint_str(" in ");
    kprint_str(function);
    kprint_str("\n");
#if CONFIG_DEBUG_BUILD
    seL4_DebugHalt();
#endif
    for (;;) {
    }
}
