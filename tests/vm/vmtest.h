/* vmtest.h - shared helpers for Fastix Phase 2+3 QEMU behavioral tests. */
#ifndef FASTIX_VMTEST_H
#define FASTIX_VMTEST_H

#include <stdint.h>

#include <fastix/abi.h>
#include <fastix/config.h>
#include <fastix_sys/sys.h>

struct vm_test_stats {
    uint64_t v[6]; /* free bytes, slots, frames, pages, vmas|pts, procs */
};

void vmtest_report(const char *name, int ok);
struct vm_test_stats vmtest_get_stats(void);
int vmtest_same_stats(struct vm_test_stats a, struct vm_test_stats b);
int64_t vmtest_wait4(int64_t pid, int *status, uint64_t options);
_Noreturn void vmtest_exit(int code);

/* Phase 3 test suites */
void run_proc_tests(void);
void run_sig_tests(void);
void run_thread_tests(void);
void run_pipe_tests(void);

#endif /* FASTIX_VMTEST_H */
