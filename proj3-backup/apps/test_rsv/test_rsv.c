#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>
#include <errno.h>
#include <string.h>

#define __NR_set_rsv    397
#define __NR_cancel_rsv 398

/* Small helper so errno is always printed consistently */
static long call_set(pid_t pid, struct timespec *C, struct timespec *T)
{
    errno = 0;
    return syscall(__NR_set_rsv, pid, C, T);
}

static long call_cancel(pid_t pid)
{
    errno = 0;
    return syscall(__NR_cancel_rsv, pid);
}

static void expect_ok(const char *label, long ret)
{
    if (ret == 0) {
        printf("%s: OK (ret=0)\n", label);
    } else {
        printf("%s: FAIL (ret=%ld, errno=%d: %s)\n",
               label, ret, errno, strerror(errno));
    }
}

static void expect_err(const char *label, long ret, int exp_errno)
{
    if (ret == -1 && errno == exp_errno) {
        printf("%s: OK (ret=-1, errno=%d: %s)\n",
               label, errno, strerror(errno));
    } else {
        printf("%s: FAIL (ret=%ld, errno=%d: %s; expected errno=%d)\n",
               label, ret, errno, strerror(errno), exp_errno);
    }
}

int main(void)
{
    struct timespec C, T;
    pid_t pid = getpid();
    long ret;

    printf("=== Testing Reservation Syscalls ===\n");
    printf("Test PID: %d\n\n", pid);

    /* ---------- Basic “happy-path” tests (what you already had) ---------- */

    C.tv_sec  = 0;
    C.tv_nsec = 10 * 1000000L;   /* 10 ms */
    T.tv_sec  = 0;
    T.tv_nsec = 100 * 1000000L;  /* 100 ms */

    /* Test 1: set reservation (should succeed) */
    ret = call_set(pid, &C, &T);
    expect_ok("Test 1: set_rsv(pid)", ret);

    /* Test 2: duplicate set (should fail with EBUSY) */
    ret = call_set(pid, &C, &T);
    expect_err("Test 2: duplicate set_rsv(pid)", ret, EBUSY);

    /* Test 3: cancel (should succeed) */
    ret = call_cancel(pid);
    expect_ok("Test 3: cancel_rsv(pid)", ret);

    /* Test 4: cancel again (should fail with EINVAL) */
    ret = call_cancel(pid);
    expect_err("Test 4: second cancel_rsv(pid)", ret, EINVAL);

    /* Test 5: same tests using pid=0 (current task) */
    printf("\n-- pid = 0 tests --\n");

    ret = call_set(0, &C, &T);
    expect_ok("set_rsv(0)", ret);

    ret = call_cancel(0);
    expect_ok("cancel_rsv(0)", ret);

    /* ---------- Extra robustness tests for 4.2 ---------- */

    printf("\n-- invalid argument tests --\n");

    /* 6: C > T should be rejected (EINVAL) */
    C.tv_sec = 0;
    C.tv_nsec = 200 * 1000000L;   /* 200 ms */
    T.tv_sec = 0;
    T.tv_nsec = 100 * 1000000L;   /* 100 ms */
    ret = call_set(pid, &C, &T);
    expect_err("Test 6: C > T", ret, EINVAL);

    /* 7: zero C (0,0) should be rejected (EINVAL) */
    C.tv_sec = 0;
    C.tv_nsec = 0;
    T.tv_sec = 0;
    T.tv_nsec = 100 * 1000000L;
    ret = call_set(pid, &C, &T);
    expect_err("Test 7: C == 0", ret, EINVAL);

    /* 8: zero T (0,0) should be rejected (EINVAL) */
    C.tv_sec = 0;
    C.tv_nsec = 10 * 1000000L;
    T.tv_sec = 0;
    T.tv_nsec = 0;
    ret = call_set(pid, &C, &T);
    expect_err("Test 8: T == 0", ret, EINVAL);

    /* 9: clearly bogus pid should give ESRCH */
    C.tv_sec = 0;
    C.tv_nsec = 10 * 1000000L;
    T.tv_sec = 0;
    T.tv_nsec = 100 * 1000000L;
    ret = call_set(999999, &C, &T);
    expect_err("Test 9: bad pid", ret, ESRCH);

    /* 10: NULL pointer for C should give EFAULT */
    ret = call_set(pid, NULL, &T);
    expect_err("Test 10: NULL C pointer", ret, EFAULT);

    /* 11: NULL pointer for T should give EFAULT */
    ret = call_set(pid, &C, NULL);
    expect_err("Test 11: NULL T pointer", ret, EFAULT);

    printf("\n=== All tests completed ===\n");
    return 0;
}
