/*
 * fastix/fs_abi.h - user-visible file system ABI: open/at flags, file modes, struct stat,
 * directory entries. Values and layouts match Linux/musl AArch64 (musl 1.2.5), so libc
 * passes them through unchanged.
 */
#ifndef FASTIX_FS_ABI_H
#define FASTIX_FS_ABI_H

#include <stdint.h>

#include <fastix/pipe_abi.h> /* FASTIX_O_NONBLOCK, FASTIX_O_CLOEXEC, F_* */
#include <fastix/time_abi.h>

/* open flags (AArch64 octal values) */
#define FASTIX_O_RDONLY     0x0000u
#define FASTIX_O_WRONLY     0x0001u
#define FASTIX_O_RDWR       0x0002u
#define FASTIX_O_ACCMODE    0x0003u
#define FASTIX_O_CREAT      0x0040u /* 0100 */
#define FASTIX_O_EXCL       0x0080u /* 0200 */
#define FASTIX_O_NOCTTY     0x0100u /* 0400 */
#define FASTIX_O_TRUNC      0x0200u /* 01000 */
#define FASTIX_O_APPEND     0x0400u /* 02000 */
#define FASTIX_O_DIRECTORY  0x4000u /* 040000 */
#define FASTIX_O_NOFOLLOW   0x8000u /* 0100000 */
#define FASTIX_O_LARGEFILE  0x20000u /* 0400000, accepted and ignored (64-bit offsets) */

/* *at() */
#define FASTIX_AT_FDCWD            (-100)
#define FASTIX_AT_SYMLINK_NOFOLLOW 0x100u
#define FASTIX_AT_REMOVEDIR        0x200u
#define FASTIX_AT_EACCESS          0x200u
#define FASTIX_AT_SYMLINK_FOLLOW   0x400u
#define FASTIX_AT_EMPTY_PATH       0x1000u

/* access() modes */
#define FASTIX_F_OK 0u
#define FASTIX_X_OK 1u
#define FASTIX_W_OK 2u
#define FASTIX_R_OK 4u

/* lseek whence */
#define FASTIX_SEEK_SET 0
#define FASTIX_SEEK_CUR 1
#define FASTIX_SEEK_END 2

/* renameat2 flags */
#define FASTIX_RENAME_NOREPLACE 1u

/* File type and permission bits (st_mode) */
#define FASTIX_S_IFMT   0170000u
#define FASTIX_S_IFIFO  0010000u
#define FASTIX_S_IFCHR  0020000u
#define FASTIX_S_IFDIR  0040000u
#define FASTIX_S_IFREG  0100000u
#define FASTIX_S_IFLNK  0120000u
#define FASTIX_S_ISUID  04000u
#define FASTIX_S_ISGID  02000u
#define FASTIX_S_ISVTX  01000u
#define FASTIX_S_IALLUGO 07777u /* all permission + set-id + sticky bits */

/* getdents64 d_type */
#define FASTIX_DT_UNKNOWN 0
#define FASTIX_DT_FIFO    1
#define FASTIX_DT_DIR     4
#define FASTIX_DT_REG     8
#define FASTIX_DT_LNK     10

struct fastix_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t pad1;
    int64_t st_size;
    int32_t st_blksize;
    int32_t pad2;
    int64_t st_blocks; /* 512-byte units */
    struct fastix_timespec st_atim;
    struct fastix_timespec st_mtim;
    struct fastix_timespec st_ctim;
    uint32_t unused[2];
};
_Static_assert(sizeof(struct fastix_stat) == 128, "struct stat must match musl AArch64");

/* struct linux_dirent64 header; the NUL-terminated name follows, records 8-byte aligned. */
struct fastix_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

#endif /* FASTIX_FS_ABI_H */
