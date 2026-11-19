/*
 * fastix/signal_abi.h - signal numbers, si_code values and wait() status
 * encoding. Numbers match musl (AArch64/generic), so libc needs no mapping.
 * The signal frame and ucontext layout are added with signal delivery (Phase 3).
 */
#ifndef FASTIX_SIGNAL_ABI_H
#define FASTIX_SIGNAL_ABI_H

#define FASTIX_SIGHUP     1
#define FASTIX_SIGINT     2
#define FASTIX_SIGQUIT    3
#define FASTIX_SIGILL     4
#define FASTIX_SIGTRAP    5
#define FASTIX_SIGABRT    6
#define FASTIX_SIGBUS     7
#define FASTIX_SIGFPE     8
#define FASTIX_SIGKILL    9
#define FASTIX_SIGUSR1    10
#define FASTIX_SIGSEGV    11
#define FASTIX_SIGUSR2    12
#define FASTIX_SIGPIPE    13
#define FASTIX_SIGALRM    14
#define FASTIX_SIGTERM    15
#define FASTIX_SIGSTKFLT  16
#define FASTIX_SIGCHLD    17
#define FASTIX_SIGCONT    18
#define FASTIX_SIGSTOP    19
#define FASTIX_SIGTSTP    20
#define FASTIX_SIGTTIN    21
#define FASTIX_SIGTTOU    22
#define FASTIX_SIGURG     23
#define FASTIX_SIGXCPU    24
#define FASTIX_SIGXFSZ    25
#define FASTIX_SIGVTALRM  26
#define FASTIX_SIGPROF    27
#define FASTIX_SIGWINCH   28
#define FASTIX_SIGIO      29
#define FASTIX_SIGPWR     30
#define FASTIX_SIGSYS     31
#define FASTIX_NSIG       65  /* signals 1..64 */

/* si_code values for synchronous faults */
#define FASTIX_ILL_ILLOPC   1
#define FASTIX_SEGV_MAPERR  1  /* address not mapped */
#define FASTIX_SEGV_ACCERR  2  /* mapped, but access not allowed */
#define FASTIX_BUS_ADRALN   1
#define FASTIX_BUS_ADRERR   2

/* wait() status words (same encoding as musl's W* macros) */
#define FASTIX_W_EXITED(code)    (((code) & 0xff) << 8)
#define FASTIX_W_SIGNALED(sig)   ((sig) & 0x7f)
#define FASTIX_W_TERMSIG(st)     ((st) & 0x7f)          /* 0 if it exited normally */
#define FASTIX_W_EXITCODE(st)    (((st) >> 8) & 0xff)

#endif /* FASTIX_SIGNAL_ABI_H */
