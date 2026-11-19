/*
 * fastix/mman.h - mmap/mprotect flag values. Identical to musl's generic
 * <sys/mman.h>, so libc passes them through unchanged.
 */
#ifndef FASTIX_MMAN_H
#define FASTIX_MMAN_H

#define FASTIX_PROT_NONE            0x0
#define FASTIX_PROT_READ            0x1
#define FASTIX_PROT_WRITE           0x2
#define FASTIX_PROT_EXEC            0x4
#define FASTIX_PROT_MASK            0x7

#define FASTIX_MAP_SHARED           0x01
#define FASTIX_MAP_PRIVATE          0x02
#define FASTIX_MAP_TYPE             0x0f
#define FASTIX_MAP_FIXED            0x10
#define FASTIX_MAP_ANONYMOUS        0x20
#define FASTIX_MAP_GROWSDOWN        0x0100
#define FASTIX_MAP_NORESERVE        0x4000
#define FASTIX_MAP_POPULATE         0x8000
#define FASTIX_MAP_STACK            0x20000
#define FASTIX_MAP_FIXED_NOREPLACE  0x100000

#endif /* FASTIX_MMAN_H */
