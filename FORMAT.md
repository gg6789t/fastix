# FORMAT.md — How Work Is Done in Fastix

This file describes the working style, code style, and reporting style used in Fastix, so any AI (or
person) produces work that looks and behaves the same. `AGENTS.md` is the rulebook (what to build and
the hard rules); this file is the style guide (how it is written). If they disagree, `AGENTS.md` wins.

---

## 1. Workflow for Every Task

1. **Read first:** `PROGRESS.md` (where we are), the relevant `AGENTS.md` sections, and the files you
   will touch.
2. **Check facts in the pinned sources, never from memory:** before calling any seL4 function, using an
   object type, or relying on a constant, `grep` the pinned headers under `third_party/` (and the
   generated ones in `build/libsel4/`). If a name or signature is not found there, it does not exist.
3. **Smallest working step:** build the simplest correct version, then build and test it right away.
   Don't write several hundred lines before the first compile.
4. **When something fails, find the cause before fixing:** read the exact error. For crashes, symbolize
   the address (`llvm-addr2line -f -e build/<binary> <pc>`). If a test fails, first decide whether the
   test or the code is wrong, and print the real values to prove it. Never "fix" a test just to make it pass.
5. **Prove the test can fail:** after a new test passes, plant a real bug in a copy of the code (or
   temporarily in the tree, then restore it) and confirm the test catches it.
6. **Clean check before reporting:** `rm -rf build build-host`, then `tools/build.sh`,
   `tools/test_host.sh`, and `python3 tests/run_qemu_tests.py`. Count the warnings; Fastix code must have zero.
7. **Update `PROGRESS.md`** (format in `AGENTS.md` rule 10) and any affected `README.md` / ADR.
8. **Report** (Section 7). **Commit only when the owner says so** (Section 6).

Keep the owner informed during long tasks with one or two plain sentences: what you finished and what
you are doing next.

---

## 2. C Code Style

### 2.1 Layout
- C17 (`-std=gnu17` for code that includes seL4 headers, `-std=c17` everywhere else); 4-space indent,
  no tabs; lines up to ~100 characters.
- Braces always, even for one-line bodies. Function opening brace on its own line; all other braces on
  the same line.
- One blank line between functions. Pointer star binds to the name: `const char *s`.

### 2.2 Every File Starts With Its Job
```c
/*
 * kobj.c - procd's kernel object allocator.
 * One or two more lines only if the reader needs the big idea (the "why").
 */
```
A `.c` file whose header already explains everything can use a one-line comment:
`/* slot_alloc.c - CNode slot bitmap allocator (see libos/slot_alloc.h). */`

### 2.3 Includes
Order: standard headers (`<stdint.h>`), then project headers in alphabetical order (`<arch/...>`,
`<fastix/...>`, `<libos/...>`, `<sel4/...>`), then local headers (`"kobj.h"`). One blank line between groups.

### 2.4 Names
- `snake_case` for everything except macros.
- Public functions are prefixed with their module: `kobj_alloc`, `vm_map`, `slot_alloc_get`, `ut_buddy_free`.
- `static` for anything used in only one file; no prefix needed.
- Macros and constants are `UPPER_CASE`. Shared constants are `FASTIX_*` and live **only** in
  `include/fastix/*.h`. A constant used by one file may be a named `#define` at the top of that file,
  with a comment.
- Header guards: `FASTIX_<NAME>_H`, `LIBOS_<NAME>_H`, `PROCD_<NAME>_H`, `ARCH_<NAME>_H`.

### 2.5 Functions
- Target ≤ 50 lines; split long ones into named steps (`build_objects`, `build_memory`, `build_thread`).
- Every public prototype gets a one-to-three-line comment saying what it does and what it returns.
- Prefer `static inline` functions over function-like macros.

### 2.6 Errors and Cleanup
- Return `0` on success, `-FASTIX_E*` (from `fastix/errno.h`) on failure. Map seL4 errors with
  `kobj_sel4_err()`.
- Mark fallible functions `LIBOS_MUST_CHECK`. Ignore a result only on purpose, written as `(void)call();`.
- Use early returns, and a single `goto out_*` cleanup chain when several resources must be released
  (see `split()` in `libs/libos/alloc/ut_buddy.c`).
- Use `libos_assert(cond)` only for "impossible" states (internal bugs), never for bad input.

### 2.7 Safety Habits
- Bounds checks never overflow: write `if (off > size || len > size - off)`, never `off + len > size`.
- Use fixed-width types (`uint64_t`, `uint32_t`) for ABI and sizes, and `1ULL << bits` for shifts.
- Use `_Static_assert` to prove layout and range facts at compile time (see `fastix/vspace.h`, `fastix/abi.h`).
- Initialize structs with designated initializers: `(struct proc){ .pid = pid, .gen = gen }`.
- Read untrusted binary data with `memcpy` into a local struct, never by casting the pointer.
- Write 64-bit addresses with all 16 hex digits: `0x0000007FF0000000ULL`.

### 2.8 Comments
Explain **why**, not what. Good: `/* A frame cap can be mapped only once, so map a temporary copy. */`
Bad: `/* copy the cap */`. No commented-out code and no `TODO` without a `PROGRESS.md` entry.

### 2.9 Example
```c
/* Returns a slot. The slot must have been handed out and not yet returned. */
void slot_alloc_put(struct slot_alloc *sa, uint64_t slot)
{
    libos_assert(slot >= sa->first && slot - sa->first < sa->count);
    uint64_t index = slot - sa->first;
    uint64_t mask = 1ULL << (index % 64);

    libos_assert((sa->bits[index / 64] & mask) != 0); /* double free */
    sa->bits[index / 64] &= ~mask;
    sa->used--;
}
```

---

## 3. Where Code Goes

| Kind of code | Location | May include seL4? |
| :--- | :--- | :--- |
| Shared constants, ABI, layouts | `include/fastix/` | No (pure C) |
| CPU-specific code | `arch/<arch>/` | Yes |
| Portable server helpers (host-testable) | `libs/libos/` | No (only `kprint.c` and `assert.c` touch seL4) |
| Server logic | `servers/<name>/` | Yes |
| Test programs that run on Fastix | `tests/kernel_ipc/`, later `tests/posix/` | Kernel tests yes, POSIX tests no |
| Host unit tests | `tests/host/test_*.c` | No |
| Decisions | `docs/adr/NNNN-short-title.md` | — |

Never edit `third_party/`. Work around upstream issues in our build files, and record the workaround in an ADR.

---

## 4. Tests

- **Host tests** (`tests/host/test_<module>.c`): built with ASan + UBSan by `tools/test_host.sh`.
  - They are model tests: run random operations, compare every result with a simple reference model,
    and check invariants after every step. At the end, free everything and require zero leaks.
  - Use 100+ seeds of the deterministic PRNG in `tests/host/rng.h` (`SEEDS 200` is normal).
  - Use a `CHECK(cond)` macro that prints `seed`, `op`, the condition, and the line, then exits 1.
  - Inject failures (e.g. a fake backend error) to test cleanup paths.
- **QEMU tests:** print markers that `tests/run_qemu_tests.py` parses:
  - `[FASTIX-TEST PASS] snake_case_name`
  - `[FASTIX-TEST FAIL] snake_case_name: reason in plain words`
  - `[FASTIX-SUITE DONE rc=N]` printed last, after which the system exits QEMU with code N.
- **Every new test is proven able to fail** (Section 1, step 5).

---

## 5. Other Files

- **Shell** (`tools/*.sh`): POSIX `sh`, `set -eu`, a usage comment at the top, clear error messages to stderr.
- **Python** (`tests/*.py`, `tools/*.py`): standard library only, `argparse`, a module docstring with usage,
  and `sys.exit(main())`.
- **CMake:** read constants from `include/fastix/` (`fastix_header_value`) instead of retyping them; set
  common flags through `fastix_sel4_target()`.
- **Markdown docs:** short sections, tables for comparisons, exact numbers with units, and the
  environment for any measurement (QEMU TCG / HVF / hardware).

---

## 6. Commits

- Only when the owner asks. Stage specific paths; never commit `build/`, `build-host/`, `.venv/`, or secrets.
- Format:
  ```
  Phase N: one-line summary in plain words

  - What changed, one bullet per area
  - How it was verified (key numbers)

  <attribution line of the AI that did the work, if any>
  ```
- After committing, show `git log --oneline -1` and confirm the working tree is clean.

---

## 7. Reports to the Owner

The owner is new to OS development and wants the project built, not lessons. Reports are:

- **Short and plain.** Lead with the result in one sentence. Define a technical word in one short
  phrase the first time it appears. No hype words.
- **Structured** as needed: what was built (a few bullets), bugs found and fixed (including your own
  mistakes, stated plainly), and any plan changes the owner should know about.
- **Always ending with:**
  - **VERIFIED:** exact commands and observed numbers.
  - **NOT VERIFIED:** what was assumed or not tested.
  - **KNOWN ISSUES:** real limitations still open.
- **Honest about measurements:** QEMU TCG numbers are "not a benchmark".
- **Ending with one clear question** when a decision is needed (e.g. "Should I commit Phase 2?").
