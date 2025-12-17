#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>
#include <errno.h>
#include <string.h>

/* Syscall numbers from the project spec */
#define __NR_set_rsv              397
#define __NR_cancel_rsv           398
#define __NR_wait_until_next_period 399

/* Simple helper to convert timespec difference to milliseconds */
static double diff_ms(const struct timespec *a, const struct timespec *b)
{
    long ds  = a->tv_sec  - b->tv_sec;
    long dns = a->tv_nsec - b->tv_nsec;
    return (double)ds * 1000.0 + (double)dns / 1.0e6;
}

int main(void)
{
    struct timespec C, T;
    long ret;
    int i;

    printf("=== 4.4 wait_until_next_period() Test ===\n\n");

    /* ------------------------------------------------------------ */
    /* Test 0: wait_until_next_period without any reservation       */
    /* ------------------------------------------------------------ */
    printf("Test 0: wait_until_next_period() with NO reservation...\n");
    errno = 0;
    ret = syscall(__NR_wait_until_next_period);
    printf("  ret = %ld, errno = %d (%s)\n\n",
           ret, errno, strerror(errno));

    /* We expect: ret < 0 and errno == EINVAL (22) */

    /* ------------------------------------------------------------ */
    /* Install a reservation on the current task (pid = 0)          */
    /* ------------------------------------------------------------ */
    C.tv_sec  = 0;
    C.tv_nsec = 20 * 1000000L;   /* 20 ms budget */
    T.tv_sec  = 0;
    T.tv_nsec = 100 * 1000000L;  /* 100 ms period */

    printf("Installing reservation on current task (pid=0):\n");
    printf("  C = %ld.%09ld sec, T = %ld.%09ld sec\n",
           C.tv_sec, C.tv_nsec, T.tv_sec, T.tv_nsec);

    errno = 0;
    ret = syscall(__NR_set_rsv, 0, &C, &T);
    if (ret != 0) {
        printf("  set_rsv failed: ret=%ld, errno=%d (%s)\n",
               ret, errno, strerror(errno));
        printf("Aborting 4.4 test.\n");
        return 1;
    }
    printf("  set_rsv(0) succeeded.\n\n");

    /* ------------------------------------------------------------ */
    /* Test 1: measure several periods using wait_until_next_period */
    /* ------------------------------------------------------------ */
    printf("Test 1: Measuring wakeup intervals with wait_until_next_period()\n");
    printf("  Expect ~100 ms between wakeups (some jitter is OK).\n\n");

    struct timespec t_prev, t_now;
    double delta;

    /* Take initial timestamp before first wait */
    clock_gettime(CLOCK_MONOTONIC, &t_prev);

    for (i = 1; i <= 5; i++) {
        errno = 0;
        ret = syscall(__NR_wait_until_next_period);
        clock_gettime(CLOCK_MONOTONIC, &t_now);

        delta = diff_ms(&t_now, &t_prev);

        printf("  Wait #%d: ret=%ld, errno=%d (%s), interval=%.2f ms\n",
               i, ret, errno, strerror(errno), delta);

        if (ret == 0) {
            /* Very rough check: between 70 ms and 130 ms is OK */
            if (delta < 70.0 || delta > 130.0) {
                printf("    WARNING: interval outside [70,130] ms window.\n");
            } else {
                printf("    OK: interval is in expected range.\n");
            }
        }

        t_prev = t_now;
    }

    printf("\n");

    /* ------------------------------------------------------------ */
    /* Test 2: cancel reservation and then call wait again          */
    /* ------------------------------------------------------------ */
    printf("Test 2: cancel_rsv(0), then call wait_until_next_period() again\n");

    errno = 0;
    ret = syscall(__NR_cancel_rsv, 0);
    printf("  cancel_rsv(0): ret=%ld, errno=%d (%s)\n",
           ret, errno, strerror(errno));

    errno = 0;
    ret = syscall(__NR_wait_until_next_period);
    printf("  wait_until_next_period() after cancel: "
           "ret=%ld, errno=%d (%s)\n",
           ret, errno, strerror(errno));

    printf("\nExpected behavior summary:\n");
    printf("  - Test 0: wait without reservation -> EINVAL\n");
    printf("  - Test 1: several waits -> ~T ms between wakeups\n");
    printf("  - Test 2: wait after cancel -> EINVAL\n");
    printf("\n=== 4.4 Test Complete ===\n");

    return 0;
}
