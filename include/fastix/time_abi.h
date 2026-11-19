/*
 * fastix/time_abi.h - timespec, itimerval, itimerspec, and sigevent layouts
 * shared between userland and procd timers. All timestamps are 64-bit (Y2038 safe).
 */
#ifndef FASTIX_TIME_ABI_H
#define FASTIX_TIME_ABI_H

#include <stdint.h>

#define FASTIX_TIMER_ABSTIME      1

#define FASTIX_ITIMER_REAL        0
#define FASTIX_ITIMER_VIRTUAL     1
#define FASTIX_ITIMER_PROF        2

#define FASTIX_SIGEV_SIGNAL       0
#define FASTIX_SIGEV_NONE         1

#define FASTIX_MAX_TIMERS_PER_PROC 32
#define FASTIX_NSEC_PER_SEC       1000000000ULL
#define FASTIX_USEC_PER_SEC       1000000ULL

struct fastix_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct fastix_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct fastix_itimerval {
    struct fastix_timeval it_interval;
    struct fastix_timeval it_value;
};

struct fastix_itimerspec {
    struct fastix_timespec it_interval;
    struct fastix_timespec it_value;
};

struct fastix_sigevent {
    uint64_t sigev_value;
    int32_t sigev_signo;
    int32_t sigev_notify;
};

#endif /* FASTIX_TIME_ABI_H */
