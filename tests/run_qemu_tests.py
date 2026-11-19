#!/usr/bin/env python3
"""Boot a Fastix image headless in QEMU and check its test markers.

Exit status 0 only if: at least one "[FASTIX-TEST PASS]", no "[FASTIX-TEST FAIL]",
"[FASTIX-SUITE DONE rc=0]" was printed, and QEMU then exited by itself with status 0.

Usage: tests/run_qemu_tests.py [--build DIR] [--timeout SECONDS] [--log FILE] [--quiet]
"""
import argparse
import json
import os
import re
import select
import subprocess
import sys
import time

PASS_RE = re.compile(r"\[FASTIX-TEST PASS\] ([^\s:]+)")
FAIL_RE = re.compile(r"\[FASTIX-TEST FAIL\] ([^\s:]+)")
DONE_RE = re.compile(r"\[FASTIX-SUITE DONE rc=(-?\d+)\]")
EXIT_GRACE_S = 5.0  # time QEMU gets to exit by itself after the DONE marker
# seL4 debug kernel messages that mean the system cannot finish; stop at once.
FATAL_RE = re.compile(r"has no fault handler|KERNEL PANIC|halting\.\.\.")


def qemu_command(build_dir):
    with open(os.path.join(build_dir, "qemu.json")) as f:
        cfg = json.load(f)
    return [
        cfg["binary"],
        "-machine", cfg["machine"],
        "-cpu", cfg["cpu"],
        "-m", str(cfg["memory_mib"]),
        "-display", "none",
        "-serial", "stdio",
        "-monitor", "none",
        "-semihosting-config", "enable=on,target=native,userspace=on",
        "-kernel", cfg["image"],
    ]


def run(cmd, timeout_s, quiet):
    """Run QEMU, return (lines, exit_code_or_None, timed_out, done_after_s, fatal_line)."""
    start = time.monotonic()
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    lines, pending = [], b""
    done_at = None
    fatal = None
    timed_out = False
    while True:
        now = time.monotonic()
        deadline = start + timeout_s if done_at is None else done_at + EXIT_GRACE_S
        if now >= deadline:
            timed_out = done_at is None
            break
        ready, _, _ = select.select([proc.stdout], [], [], deadline - now)
        if not ready:
            continue
        chunk = os.read(proc.stdout.fileno(), 4096)
        if not chunk:  # QEMU exited and closed its output
            break
        pending += chunk
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            line = raw.decode("utf-8", "replace").rstrip("\r")
            lines.append(line)
            if not quiet:
                print(line, flush=True)
            if done_at is None and DONE_RE.search(line):
                done_at = time.monotonic()
            if fatal is None and FATAL_RE.search(line):
                fatal = line
                done_at = time.monotonic()  # collect a few more lines, then stop
    if pending:
        lines.append(pending.decode("utf-8", "replace").rstrip("\r"))
    try:
        exit_code = proc.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        exit_code = None  # did not exit by itself
    done_after = None if done_at is None else done_at - start
    return lines, exit_code, timed_out, done_after, fatal


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", default=os.path.join(repo, "build"), help="build directory")
    ap.add_argument("--timeout", type=float, default=60.0, help="seconds before giving up")
    ap.add_argument("--log", help="also write the serial log to this file")
    ap.add_argument("--quiet", action="store_true", help="do not echo serial output")
    args = ap.parse_args()

    lines, exit_code, timed_out, done_after, fatal = run(qemu_command(args.build), args.timeout,
                                                  args.quiet)
    if args.log:
        with open(args.log, "w") as f:
            f.write("\n".join(lines) + "\n")

    passed = [m.group(1) for line in lines for m in [PASS_RE.search(line)] if m]
    failed = [m.group(1) for line in lines for m in [FAIL_RE.search(line)] if m]
    done = [int(m.group(1)) for line in lines for m in [DONE_RE.search(line)] if m]

    problems = []
    if fatal:
        problems.append("seL4 reported a fatal error: " + fatal.strip())
    if timed_out:
        problems.append(f"timed out after {args.timeout:.0f}s without a DONE marker")
    if not done:
        problems.append("no [FASTIX-SUITE DONE] marker")
    elif done[-1] != 0:
        problems.append(f"suite reported rc={done[-1]}")
    if failed:
        problems.append("failed tests: " + ", ".join(failed))
    if not passed:
        problems.append("no passing tests")
    if exit_code is None:
        problems.append("QEMU did not exit by itself (semihosting exit not reached)")
    elif exit_code != 0:
        problems.append(f"QEMU exited with status {exit_code}")

    print("---")
    print(f"tests passed: {len(passed)} ({', '.join(passed) or '-'})")
    print(f"tests failed: {len(failed)}")
    if done_after is not None:
        print(f"host time to DONE marker: {done_after:.3f}s (QEMU TCG, includes QEMU startup)")
    print(f"QEMU exit status: {exit_code}")
    if problems:
        print("RESULT: FAIL - " + "; ".join(problems))
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
