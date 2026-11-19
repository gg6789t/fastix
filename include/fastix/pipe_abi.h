/*
 * fastix/pipe_abi.h - flag constants for pipe2, eventfd2, dup3, and fcntl.
 * Values match Linux/musl AArch64 so libc passes them through unchanged.
 */
#ifndef FASTIX_PIPE_ABI_H
#define FASTIX_PIPE_ABI_H

/* Open / pipe2 / dup3 flags */
#define FASTIX_O_NONBLOCK      0x00000800u /* 04000 octal */
#define FASTIX_O_CLOEXEC       0x00080000u /* 02000000 octal */

/* eventfd2 flags */
#define FASTIX_EFD_SEMAPHORE   0x00000001u
#define FASTIX_EFD_NONBLOCK    FASTIX_O_NONBLOCK
#define FASTIX_EFD_CLOEXEC     FASTIX_O_CLOEXEC

/* fcntl commands & flags */
#define FASTIX_F_DUPFD         0
#define FASTIX_F_GETFD         1
#define FASTIX_F_SETFD         2
#define FASTIX_F_GETFL         3
#define FASTIX_F_SETFL         4
#define FASTIX_F_DUPFD_CLOEXEC 1030

#define FASTIX_FD_CLOEXEC      1u

#endif /* FASTIX_PIPE_ABI_H */
