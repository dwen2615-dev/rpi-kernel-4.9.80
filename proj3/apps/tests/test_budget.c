#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <sched.h>
#include <signal.h>

/* Syscall numbers for project 3 */
#define __NR_set_rsv                  397
#define __NR_cancel_rsv               398
#define __NR_wait_until_next_period   399

/* --------- helpers --------- */

static const char *policy_name(int pol)
{
    switch (pol) {
    case SCHED_OTHER: return "SCHED_OTHER";
    case SCHED_FIFO:  return "SCHED_FIFO";
    case SCHED_RR:    return "SCHED_RR";
    default:          return "UNKNOWN";
    }
}

static double diff_ms(const struct timespec *start,
                      const struct timespec *end)
{
    double s  = (double)(end->tv_sec  - start->tv_sec);
    double ns = (double)(end->tv_nsec - start->tv_nsec);
    return s * 1000.0 + ns / 1.0e6;
}

/* Busy-loop for approx ms milliseconds of CPU time */
static void busy_for_ms(double ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (diff_ms(&start, &now) >= ms)
            break;
        /* prevent compiler from optimizing loop away */
        asm volatile("" ::: "memory");
    }
}

/* Print current scheduling policy and RT priority */
static void print_sched_info(const char *label, pid_t pid)
{
    int pol = sched_getscheduler(pid);
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sched_getparam(pid, &sp);

    printf("%s: pid=%d policy=%s (%d) rt_prio=%d\n",
           label, (int)pid, policy_name(pol), pol, sp.sched_priority);
}

/* Thin wrappers for syscalls */
static long do_set_rsv(pid_t pid, const struct timespec *C,
                       const struct timespec *T)
{
    return syscall(__NR_set_rsv, pid, C, T);
}

static long do_cancel_rsv(pid_t pid)
{
    return syscall(__NR_cancel_rsv, pid);
}

static long do_wait_until_next_period(void)
{
    return syscall(__NR_wait_until_next_period);
}

/* --------- tests --------- */

static void test_single_task_overrun(void)
{
    struct timespec C, T;
    long ret;
    struct timespec t0, t1;

    printf("=== 4.5 Budget Overrun Test: single task ===\n\n");

    /* Test 0: make sure wait_until_next_period() rejects no-reservation */
    printf("Test 0: wait_until_next_period() with NO reservation:\n");
    ret = do_wait_until_next_period();
    printf("  ret=%ld errno=%d (%s)\n\n", ret, errno, strerror(errno));

    /* Set reservation on current task */
    C.tv_sec  = 0;
    C.tv_nsec = 20 * 1000000L;   /* 20 ms budget */
    T.tv_sec  = 0;
    T.tv_nsec = 100 * 1000000L;  /* 100 ms period */

    printf("Installing reservation on current task (pid=%d):\n", (int)getpid());
    printf("  C = 20 ms, T = 100 ms\n");
    ret = do_set_rsv(0, &C, &T);
    if (ret < 0) {
        printf("  set_rsv(0) FAILED: ret=%ld errno=%d (%s)\n\n",
               ret, errno, strerror(errno));
        return;
    }
    printf("  set_rsv(0) succeeded.\n\n");

    /* Align to period start */
    printf("Syncing to start of a period with wait_until_next_period()...\n");
    ret = do_wait_until_next_period();
    if (ret < 0) {
        printf("  wait_until_next_period FAILED: ret=%ld errno=%d (%s)\n\n",
               ret, errno, strerror(errno));
        do_cancel_rsv(0);
        return;
    }
    printf("  Woke up at start of period.\n\n");

    /* At this point we should be RT */
    print_sched_info("[Before overrun]", getpid());
    printf("  (EXPECTED: SCHED_FIFO with positive RT priority)\n\n");

    /* Now burn MORE than C ms of CPU in this period */
    printf("Test 1: busy for 60 ms ( > C = 20 ms ), should trigger OVERRUN\n");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    busy_for_ms(60.0);   /* more conservative than 40 ms */
    clock_gettime(CLOCK_MONOTONIC, &t1);

    printf("  busy_for_ms() actual elapsed ~%.2f ms\n", diff_ms(&t0, &t1));

    /* Yield so scheduler can apply demotion if needed, then small pause */
    sched_yield();
    usleep(10000);   /* 10 ms – gives scheduler time to run others */

    print_sched_info("[After overrun in same period]", getpid());
    printf("  (EXPECTED: SCHED_OTHER, priority 0)\n\n");

    /* Next period: budget should reset and RT priority restored */
    printf("Test 2: wait_until_next_period(), expect RT priority to return\n");
    ret = do_wait_until_next_period();
    if (ret < 0) {
        printf("  wait_until_next_period FAILED: ret=%ld errno=%d (%s)\n",
               ret, errno, strerror(errno));
        do_cancel_rsv(0);
        return;
    }

    print_sched_info("[After next period release]", getpid());
    printf("  (EXPECTED: back to SCHED_FIFO with RM priority)\n\n");

    /* Clean up */
    ret = do_cancel_rsv(0);
    printf("cancel_rsv(0): ret=%ld errno=%d (%s)\n\n",
           ret, errno, strerror(errno));

    printf("NOTE: You should also see a kernel log line like:\n");
    printf("  \"Task <pid>: budget overrun (util: xx %%)\"\n");
    printf("Run on Pi: dmesg | tail -n 20\n\n");
}

int main(void)
{
    printf("===============================================\n");
    printf(" Project 3 – 4.5 Budget Accounting Test\n");
    printf("===============================================\n\n");

    test_single_task_overrun();

    printf("=== 4.5 Budget Test Complete ===\n");
    return 0;
}
