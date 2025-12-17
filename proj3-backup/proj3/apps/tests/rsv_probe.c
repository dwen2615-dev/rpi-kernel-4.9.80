#include <stdio.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

#define __NR_set_rsv 397
#define __NR_cancel_rsv 398

int main(void) {
    struct timespec C = { .tv_sec = 0, .tv_nsec = 5*1000000L };   // 5 ms
    struct timespec T = { .tv_sec = 0, .tv_nsec = 20*1000000L };  // 20 ms

    long r = syscall(__NR_set_rsv, 0, &C, &T);
    printf("set_rsv -> %ld\n", r);

    r = syscall(__NR_cancel_rsv, 0);
    printf("cancel_rsv -> %ld\n", r);

    return 0;
}

