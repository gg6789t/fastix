# ADR 0003: How `vfsd` starts, reaches client memory, and shares the FD table with `procd`

Status: accepted (2026-11-19, Phase 4C)

## Context

`vfsd` (rank 30) owns file descriptor (FD) numbers and open file descriptions (OFDs) for
every process (AGENTS.md 6.4). It needs memory, it must read paths and data from client
processes, and `procd` must still serve `read`/`write` on pipes and eventfds, which it owns,
without calling `vfsd` (procd never calls upward, 14.5).

## Decisions

1. **`vfsd` is a system process run by `procd`.** `procd` starts it from its embedded image
   with the normal ELF loader, right after creating PID 1 (which waits for it, see 5). `vfsd` therefore gets demand-paged memory (it
   calls `mmap` like any program), `procd` handles its page faults, and shared memory with
   clients uses `procd`'s existing `anon_shm` objects. It is marked as a server: user
   processes cannot signal it. This replaces a hand-built bootstrap like `devd`'s and reuses
   tested code. All its calls to `procd` (faults, `mmap`, server operations) go downward.

2. **Client memory through the I/O window (AGENTS.md 5.3, 5.5).** Each process has one
   shared-memory object covering all its per-thread I/O windows (`FASTIX_IOWIN_BASE`, 64 MB of
   address space, pages created on first touch). It is mapped in the process and, at a
   per-client address, in `vfsd`. Every pointer passed to `vfsd` must point into the caller's
   own window; `vfsd` rejects anything else with `EFAULT`. `libfastix_sys` copies paths and
   data through the window.

3. **Exported FD table instead of per-server mirrors (simplifies 6.4 rule 3).** `vfsd` keeps
   its own FD table and also writes one 32-bit word per FD (`owner << 28 | object index`, 0 =
   closed) into one page per process that `procd` maps read-only. `procd` reads the word
   directly (no mirror, no stale-FD round trip) to serve `read`/`write` on pipe and eventfd
   FDs. Each word is written with one atomic store, so `procd` never sees a half-written entry.
   `vfsd` clears the word before it tells `procd` to free the object, so an object `procd`
   finds through the table is always alive. If an FD belongs to another server, the
   server answers `-FASTIX_ESTALEFD` with the owner in `MR1`, and `libfastix_sys` retries once
   at the owner. This keeps rules 1, 2, 4, 5, 6 of 6.4 unchanged and is just as fast.

4. **`procd` → `vfsd` work queue pulled with downward calls (instead of a shared ring).**
   `procd` queues work items (new client, exec, exit, install FDs) in its own memory and
   signals `vfsd`'s notification. `vfsd` fetches items with the downward call
   `FASTIX_SRV_VFS_WORK`, which also returns the result of the previous item. `procd` never
   blocks and needs no ring-overflow handling. It costs one IPC per item, only on `fork`,
   `exec`, `exit`, `pipe2` and `eventfd2`.

5. **A new program starts only after `vfsd` has set up its FD table.** For `fork`, `vfork`,
   `posix_spawn`, `execve` and PID 1, `procd` parks the caller (and holds the new main thread)
   until `vfsd` acknowledges the work item, so no FD call can overtake the table copy.

6. **`tmpfs` moves from 4D into 4C** as the root filesystem, because the path walker and FD
   table need a real filesystem to be tested. File data lives in `vfsd`'s own memory until
   the unified page cache (4E).

## Consequences

- Each process costs one extra page (the exported FD table); 1024 FDs per process in 4C.
- `vfsd` appears in the process table as PID 2 (like QNX servers); PID 1 stays init.
- `procd` maps a client's window, info page and FD-table pages into `vfsd` when `vfsd` fetches
  the client's NEW item, and removes them when `vfsd` acknowledges its EXIT item, so `vfsd`
  never touches an unmapped client page.
- If `vfsd` crashes, files are lost (no restart yet); `procd` reports it.
