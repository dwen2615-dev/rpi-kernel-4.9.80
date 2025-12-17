#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>
#include <string.h>

#define __NR_set_rsv 397

int main(void) {
    printf("=== Testing Exit Cleanup ===\n");

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }

    if (child == 0) {
        struct timespec C = {0, 5 * 1000000L};   /* 5 ms */
        struct timespec T = {0, 20 * 1000000L};  /* 20 ms */

        printf("[child %d] calling set_rsv(0)...\n", getpid());
        long ret = syscall(__NR_set_rsv, 0, &C, &T);
        printf("[child] set_rsv ret=%ld errno=%d (%s)\n",
               ret, errno, strerror(errno));
        printf("[child] exiting WITHOUT cancel_rsv()\n");
        _exit(0);
    } else {
        int status;
        waitpid(child, &status, 0);
        printf("[parent] child %d exited, status=%d\n", child, status);
        printf("[parent] if you added printk() in rsv_cleanup_task, "
               "check dmesg now.\n");
    }

    return 0;
}
