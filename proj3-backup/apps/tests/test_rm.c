#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sched.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>

#define __NR_set_rsv 397
#define __NR_cancel_rsv 398

// Helper: print current scheduling policy + priority
void print_sched_info(const char *label, pid_t pid) {
    int policy = sched_getscheduler(pid);
    struct sched_param sp;

    if (sched_getparam(pid, &sp) != 0) {
        printf("%s: sched_getparam failed (pid=%d, errno=%d: %s)\n",
               label, pid, errno, strerror(errno));
        return;
    }

    const char *pname =
        (policy == SCHED_FIFO) ? "SCHED_FIFO" :
        (policy == SCHED_RR)   ? "SCHED_RR" :
        (policy == SCHED_OTHER)? "SCHED_OTHER" :
                                 "UNKNOWN";

    printf("%s: pid=%d  policy=%s  rt_prio=%d\n",
           label, pid, pname, sp.sched_priority);
}

int main(void) {
    printf("=== 4.3 RM Priority Test ===\n");

    pid_t childA = fork();
    if (childA == 0) {
        // Child A: shorter period => should get HIGHER RT priority
        struct timespec C = {0, 5 * 1000000L};    // 5 ms
        struct timespec T = {0, 20 * 1000000L};   // 20 ms (shorter)

        long ret = syscall(__NR_set_rsv, 0, &C, &T);
        if (ret != 0) {
            printf("[A] set_rsv failed (ret=%ld errno=%d: %s)\n",
                   ret, errno, strerror(errno));
            _exit(1);
        }

        print_sched_info("[A] After set_rsv", getpid());
        sleep(10); // keep alive so parent can inspect
        syscall(__NR_cancel_rsv, 0);
        _exit(0);
    }

    pid_t childB = fork();
    if (childB == 0) {
        // Child B: longer period => should get LOWER RT priority
        struct timespec C = {0, 5 * 1000000L};    // 5 ms
        struct timespec T = {0, 100 * 1000000L};  // 100 ms (longer)

        long ret = syscall(__NR_set_rsv, 0, &C, &T);
        if (ret != 0) {
            printf("[B] set_rsv failed (ret=%ld errno=%d: %s)\n",
                   ret, errno, strerror(errno));
            _exit(1);
        }

        print_sched_info("[B] After set_rsv", getpid());
        sleep(10);
        syscall(__NR_cancel_rsv, 0);
        _exit(0);
    }

    // Parent: give children time to call set_rsv()
    sleep(1);

    printf("\n=== Parent checking children ===\n");
    print_sched_info("Child A info", childA);
    print_sched_info("Child B info", childB);

    printf("\nEXPECTED:\n");
    printf("  Child A (T = 20ms) should have HIGHER RT priority than Child B (T = 100ms)\n");
    printf("  In Linux RT, HIGHER priority = larger sched_priority number.\n\n");

    waitpid(childA, NULL, 0);
    waitpid(childB, NULL, 0);

    printf("=== RM Priority Test Complete ===\n");
    return 0;
}
