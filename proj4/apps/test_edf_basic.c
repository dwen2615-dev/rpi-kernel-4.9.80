#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

#include "edf_api.h"

#ifndef SYS_gettid
#define SYS_gettid 224   /* Fallback; usually defined in <sys/syscall.h> */
#endif

#define DUMMY_LOAD_ITER 1000
static int dummy_load_calib = 1000;

/* Test counters */
static int test_passed = 0;
static int test_failed = 0;

/* Busy CPU load (approx) */
static void dummy_load(int load_ms)
{
    int i, j;
    for (j = 0; j < dummy_load_calib * load_ms; j++) {
        for (i = 0; i < DUMMY_LOAD_ITER; i++)
            __asm__ volatile ("nop");
    }
}

static void print_test_result(const char *test_name, int ok)
{
    if (ok) {
        printf("[PASS] %s\n", test_name);
        test_passed++;
    } else {
        int err = errno;
        printf("[FAIL] %s (errno=%d: %s)\n",
               test_name, err, strerror(err));
        test_failed++;
    }
}

/* Task parameters */
struct task_params {
    int task_id;
    int C_ms;
    int T_ms;
    int cpu_id;
    int chain_id;
    int chain_pos;
    int num_jobs;
};

/* Thread body for EDF task */
static void *edf_task_thread(void *arg)
{
    struct task_params *params = (struct task_params *)arg;
    pid_t tid = syscall(SYS_gettid);
    struct timespec C, T, D;
    int job_count = 0;
    struct sched_param sp;
    int policy;

    printf("  [Task %d] TID=%d C=%dms T=%dms cpu=%d chain=%d pos=%d\n",
           params->task_id, tid, params->C_ms, params->T_ms,
           params->cpu_id, params->chain_id, params->chain_pos);

    /* C, T, D=T */
    C.tv_sec  = params->C_ms / 1000;
    C.tv_nsec = (params->C_ms % 1000) * 1000000L;

    T.tv_sec  = params->T_ms / 1000;
    T.tv_nsec = (params->T_ms % 1000) * 1000000L;

    D = T;   /* D = T as per spec */

    int ret = set_edf_task(tid, &C, &T, &D,
                           params->cpu_id, params->chain_id, params->chain_pos);
    if (ret < 0) {
        printf("  [Task %d] set_edf_task FAILED: ret=%d errno=%d (%s)\n",
               params->task_id, ret, errno, strerror(errno));
        return (void *)-1;
    }

    /* Check scheduling policy and RT priority */
    if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0) {
        const char *pname =
            (policy == SCHED_FIFO) ? "SCHED_FIFO" :
            (policy == SCHED_RR)   ? "SCHED_RR" :
            (policy == SCHED_OTHER)? "SCHED_OTHER" : "UNKNOWN";
        printf("  [Task %d] Policy=%s, RT prio=%d\n",
               params->task_id, pname, sp.sched_priority);
    }

    /* Check CPU affinity */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (pthread_getaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0) {
        printf("  [Task %d] CPU%d affinity: %s\n",
               params->task_id, params->cpu_id,
               CPU_ISSET(params->cpu_id, &cpuset) ? "YES" : "NO");
    }

    /* Do periodic jobs using ~80% of C */
    int load_ms = (params->C_ms * 80) / 100;
    while (job_count < params->num_jobs) {
        dummy_load(load_ms);

        ret = wait_until_next_period();
        if (ret < 0) {
            printf("  [Task %d] wait_until_next_period failed at job %d: %s\n",
                   params->task_id, job_count, strerror(errno));
            break;
        }

        job_count++;
        if (job_count % 10 == 0) {
            printf("  [Task %d] jobs %d/%d\n",
                   params->task_id, job_count, params->num_jobs);
        }
    }

    printf("  [Task %d] exiting after %d jobs\n",
           params->task_id, job_count);

    /* Cancel reservation */
    ret = cancel_rsv(tid);
    if (ret < 0) {
        printf("  [Task %d] WARNING: cancel_rsv failed: %s\n",
               params->task_id, strerror(errno));
    }

    return (job_count == params->num_jobs) ? (void *)0 : (void *)-1;
}

/* Test 1: Invalid parameters */
static void test_invalid_parameters(void)
{
    printf("\n=== Test 1: Invalid parameters ===\n");

    pid_t pid = getpid();
    struct timespec C, T, D;

    C.tv_sec  = 0;
    C.tv_nsec = 50 * 1000000L;
    T.tv_sec  = 0;
    T.tv_nsec = 100 * 1000000L;
    D = T;

    int ret;

    errno = 0;
    ret = set_edf_task(pid, &C, &T, &D, 5, 0, 0);
    print_test_result("Reject invalid cpu_id=5", (ret < 0 && errno == EINVAL));

    errno = 0;
    ret = set_edf_task(pid, &C, &T, &D, 0, 5, 0);
    print_test_result("Reject invalid chain_id=5", (ret < 0 && errno == EINVAL));

    errno = 0;
    ret = set_edf_task(pid, &C, &T, &D, 0, 0, 5);
    print_test_result("Reject invalid chain_pos=5", (ret < 0 && errno == EINVAL));

    errno = 0;
    ret = set_edf_task(pid, NULL, &T, &D, 0, 0, 0);
    print_test_result("Reject NULL C pointer", (ret < 0 && errno == EFAULT));
}

/* Test 2: Single task on CPU 0 */
static void test_single_task_cpu0(void)
{
    printf("\n=== Test 2: Single task on CPU 0 ===\n");

    pthread_t thread;
    struct task_params params = {
        .task_id   = 1,
        .C_ms      = 50,
        .T_ms      = 200,
        .cpu_id    = 0,
        .chain_id  = 0,
        .chain_pos = 0,
        .num_jobs  = 20
    };

    int ret = pthread_create(&thread, NULL, edf_task_thread, &params);
    if (ret != 0) {
        print_test_result("Thread create (single task)", 0);
        return;
    }

    void *tret;
    pthread_join(thread, &tret);
    print_test_result("Single task on CPU 0", ((long)tret == 0));
}

/* Test 3: Two tasks on different CPUs */
static void test_two_tasks_different_cpus(void)
{
    printf("\n=== Test 3: Two tasks on different CPUs ===\n");

    pthread_t threads[2];
    struct task_params params[2] = {
        { .task_id=1, .C_ms=30, .T_ms=100, .cpu_id=0, .chain_id=0, .chain_pos=0, .num_jobs=20 },
        { .task_id=2, .C_ms=40, .T_ms=150, .cpu_id=1, .chain_id=1, .chain_pos=0, .num_jobs=15 }
    };

    for (int i = 0; i < 2; i++) {
        int ret = pthread_create(&threads[i], NULL, edf_task_thread, &params[i]);
        if (ret != 0)
            printf("  Failed to create task %d: %s\n", i+1, strerror(ret));
        usleep(100000);
    }

    int ok = 1;
    for (int i = 0; i < 2; i++) {
        void *tret;
        pthread_join(threads[i], &tret);
        if ((long)tret != 0)
            ok = 0;
    }

    print_test_result("Two tasks on different CPUs", ok);
}

/* Test 4: Three tasks on same CPU (EDF ordering) */
static void test_three_tasks_same_cpu_edf(void)
{
    printf("\n=== Test 4: Three tasks on same CPU (EDF priority) ===\n");
    printf("Expect: shortest T (50ms) should get highest RT priority.\n");

    pthread_t threads[3];
    struct task_params params[3] = {
        { .task_id=1, .C_ms=20, .T_ms=100, .cpu_id=0, .chain_id=0, .chain_pos=0, .num_jobs=15 },
        { .task_id=2, .C_ms=15, .T_ms=50,  .cpu_id=0, .chain_id=1, .chain_pos=0, .num_jobs=30 },
        { .task_id=3, .C_ms=25, .T_ms=200, .cpu_id=0, .chain_id=2, .chain_pos=0, .num_jobs=10 },
    };

    for (int i = 0; i < 3; i++) {
        int ret = pthread_create(&threads[i], NULL, edf_task_thread, &params[i]);
        if (ret != 0)
            printf("  Failed to create task %d: %s\n", i+1, strerror(ret));
        usleep(100000);
    }

    int ok = 1;
    for (int i = 0; i < 3; i++) {
        void *tret;
        pthread_join(threads[i], &tret);
        if ((long)tret != 0)
            ok = 0;
    }

    print_test_result("Three tasks with EDF priorities", ok);
}

int main(void)
{
    printf("========================================\n");
    printf("  EDF Scheduler Test Suite (Section 4.1)\n");
    printf("========================================\n");
    printf("Using syscalls: 397=set_edf_task, 398=cancel_rsv, 399=wait_until_next_period\n\n");

    test_invalid_parameters();
    test_single_task_cpu0();
    test_two_tasks_different_cpus();
    test_three_tasks_same_cpu_edf();

    printf("\n========================================\n");
    printf("  Test Summary\n");
    printf("========================================\n");
    printf("Passed: %d\n", test_passed);
    printf("Failed: %d\n", test_failed);
    printf("Total:  %d\n", test_passed + test_failed);
    printf("========================================\n");
    printf("\nCheck dmesg for EDF priority logs.\n");

    return (test_failed == 0) ? 0 : 1;
}
