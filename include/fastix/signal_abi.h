/*
 * fastix/signal_abi.h - signal numbers, flags, si_code values, wait() status
 * encoding, and the user-stack signal frame / VDSO trampoline ABI.
 * Numbers match musl (AArch64/generic) so libc needs no translation table.
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
#define FASTIX_SIGRTMIN   32
#define FASTIX_SIGRTMAX   64
#define FASTIX_NSIG       65  /* signals 1..64 */

/* Special handler values */
#define FASTIX_SIG_DFL    0ULL
#define FASTIX_SIG_IGN    1ULL

/* sigaction flags (match musl <signal.h>) */
#define FASTIX_SA_NOCLDSTOP 0x00000001ULL
#define FASTIX_SA_NOCLDWAIT 0x00000002ULL
#define FASTIX_SA_SIGINFO   0x00000004ULL
#define FASTIX_SA_ONSTACK   0x08000000ULL
#define FASTIX_SA_RESTART   0x10000000ULL
#define FASTIX_SA_NODEFER   0x40000000ULL
#define FASTIX_SA_RESETHAND 0x80000000ULL

/* sigprocmask how values */
#define FASTIX_SIG_BLOCK    0
#define FASTIX_SIG_UNBLOCK  1
#define FASTIX_SIG_SETMASK  2

/* sigaltstack flags & minimum size */
#define FASTIX_SS_ONSTACK   1
#define FASTIX_SS_DISABLE   2
#define FASTIX_MINSIGSTKSZ  2048

/* si_code values */
#define FASTIX_SI_USER      0
#define FASTIX_SI_QUEUE     (-1)
#define FASTIX_SI_TIMER     (-2)
#define FASTIX_SI_TKILL     (-6)

#define FASTIX_ILL_ILLOPC   1
#define FASTIX_SEGV_MAPERR  1  /* address not mapped */
#define FASTIX_SEGV_ACCERR  2  /* mapped, but access not allowed */
#define FASTIX_BUS_ADRALN   1
#define FASTIX_BUS_ADRERR   2

#define FASTIX_CLD_EXITED    1
#define FASTIX_CLD_KILLED    2
#define FASTIX_CLD_STOPPED   5
#define FASTIX_CLD_CONTINUED 6

/* wait() status words (same encoding as musl's W* macros) */
#define FASTIX_W_EXITED(code)    (((code) & 0xff) << 8)
#define FASTIX_W_SIGNALED(sig)   ((sig) & 0x7f)
#define FASTIX_W_STOPPED(sig)    ((((sig) & 0xff) << 8) | 0x7f)
#define FASTIX_W_CONTINUED       0xffff
#define FASTIX_W_TERMSIG(st)     ((st) & 0x7f)          /* 0 if it exited normally */
#define FASTIX_W_EXITCODE(st)    (((st) >> 8) & 0xff)
#define FASTIX_W_IFSTOPPED(st)   (((st) & 0xff) == 0x7f)
#define FASTIX_W_STOPSIG(st)     (((st) >> 8) & 0xff)
#define FASTIX_W_IFCONTINUED(st) ((st) == 0xffff)

/* Fixed VDSO code page offsets & signal frame offsets shared with assembly. */
#define FASTIX_VDSO_SIGENTER_OFF  0x100
#define FASTIX_VDSO_SIGRETURN_OFF 0x200
#define FASTIX_SIGFRAME_EP_OFF    16
#define FASTIX_SIGFRAME_FPU_OFF   416
#define FASTIX_SIGFRAME_MAGIC     0x465853494746524DULL

/* seL4_MessageInfo word for FASTIX_SYS_rt_sigreturn (label=85, length=1). */
#define FASTIX_SIGRETURN_MSGINFO  ((85 << 12) | 1)

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

struct fastix_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct fastix_sigaltstack {
    uint64_t ss_sp;
    int32_t ss_flags;
    uint32_t pad;
    uint64_t ss_size;
};

struct fastix_siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    uint32_t pad;
    uint32_t si_pid;
    uint32_t si_uid;
    int32_t si_status;
    int32_t si_timerid;
    uint64_t si_addr;
    uint64_t si_value;
};

/* AArch64 NEON/FP state saved & restored in user mode by the VDSO trampoline. */
struct fastix_fpu_state {
    uint64_t v[64]; /* q0..q31: 32 x 128-bit = 512 bytes */
    uint32_t fpsr;
    uint32_t fpcr;
    uint64_t pad;
};

struct fastix_sigcontext {
    uint64_t x[31]; /* x0..x30 */
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t tpidr_el0;
    uint64_t fault_addr;
    struct fastix_fpu_state fpu;
};

struct fastix_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct fastix_sigaltstack uc_stack;
    uint64_t uc_sigmask;
    struct fastix_sigcontext uc_mcontext;
};

struct fastix_sigframe {
    uint64_t magic;
    uint64_t handler;
    uint64_t procd_ep;
    uint64_t saved_on_altstack;
    struct fastix_siginfo info;
    struct fastix_ucontext uc;
};

static inline uint64_t fastix_sig_bit(int sig)
{
    return 1ULL << (unsigned)(sig - 1);
}

_Static_assert(offsetof(struct fastix_sigframe, procd_ep) == FASTIX_SIGFRAME_EP_OFF,
               "sigframe procd_ep offset mismatch");
_Static_assert(offsetof(struct fastix_sigframe, uc) +
                   offsetof(struct fastix_ucontext, uc_mcontext) +
                   offsetof(struct fastix_sigcontext, fpu) == FASTIX_SIGFRAME_FPU_OFF,
               "sigframe FPU offset mismatch");
_Static_assert((FASTIX_SIGFRAME_FPU_OFF & 15) == 0, "FPU state must be 16-byte aligned");

#endif /* !__ASSEMBLER__ */

#endif /* FASTIX_SIGNAL_ABI_H */
