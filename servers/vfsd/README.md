# vfsd

The VFS server (AGENTS.md 6.3 C, IPC rank 30). It owns every process's FD numbers and open
file descriptions, the mount table, the path walker and the root `tmpfs`. Design decisions:
`docs/adr/0003-vfsd-core.md`.

- **How it runs:** `procd` starts it as a system process (PID 2) right after creating PID 1.
  Its memory is demand-paged by `procd`; user processes cannot signal it.
- **Client calls** (label = `FASTIX_SYS_*`, Linux argument order, pointers must point into
  the caller's I/O window): `openat`, `close`, `read`, `write`, `pread64`, `pwrite64`, `lseek`,
  `dup`, `dup3`, `fcntl` (`DUPFD`, `GETFD/SETFD`, `GETFL/SETFL`), `fstat`, `newfstatat`,
  `getdents64`, `ftruncate`, `truncate`, `fchmod`, `fchmodat`, `faccessat`, `mkdirat`,
  `unlinkat`, `symlinkat`, `readlinkat`, `linkat`, `renameat2` (`RENAME_NOREPLACE`), `chdir`,
  `fchdir`, `getcwd`, `mount`/`umount2` (tmpfs, root only). `read`/`write` on a pipe or eventfd
  answer `-FASTIX_ESTALEFD` with `MR1` = owner, and the caller retries at `procd`.
- **From procd:** work items NEW, EXEC, EXIT, INSTALL, pulled with `FASTIX_SRV_VFS_WORK` when
  `procd` signals; `vfsd` tells `procd` with `FASTIX_SRV_OBJ_RELEASE` / `FASTIX_SRV_OBJ_SETFL`
  when a pipe/eventfd OFD is closed for the last time or changes `O_NONBLOCK`.
- **Files:** `main.c` (loop, memory, I/O window checks), `client.c` (clients, FD tables, OFDs,
  exported FD table), `fs_path.c` / `fs_file.c` (the calls), `namei.c` (path walk), `vnode.c`
  (references, permissions, mounts), `tmpfs.c` (in-memory filesystem).
- **Data structures:** client table (one per process slot, 1024 FDs each), slab-allocated OFDs
  and vnodes, tmpfs directories as lists of entries, file data in radix trees of 4 KB pages.
- **Locks / rank:** single-threaded, no locks; calls only `procd` (downward).
- **Tests:** `tests/vm/test_vfs.c`, `tests/vm/test_vfs2.c` (QEMU) and `tests/host/test_namei.c`
  (200 seeds compared against the host file system).
