/*
 * EDF Loader - Section 4.2
 * Reads taskset.txt, partitions tasks using FFD, runs for 2+ minutes
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <errno.h>

#include "edf_api.h"

#define MAX_TASKS 16
#define NUM_CPUS  2
#define RUN_TIME_SEC 120  /* Run for 2 minutes */

/* Task definition from taskset.txt */
struct task_def {
    int C_ms;       /* Computation time in ms */
    int T_ms;       /* Period in ms */
    int D_ms;       /* Deadline in ms */
    int chain_id;
    int chain_pos;
    double util;    /* C/T */
    int cpu_id;     /* Assigned CPU (-1 = unassigned) */
    int task_idx;   /* Original index */
};

/* Runtime task info */
struct task_runtime {
    struct task_def *def;
    pthread_t thread;
    pid_t tid;
    volatile int running;
    int jobs_completed;
};

static struct task_def tasks[MAX_TASKS];
static struct task_runtime task_rt[MAX_TASKS];
static int num_tasks = 0;
static volatile int global_stop = 0;

/* Compare function for sorting by utilization (descending) */
static int cmp_util_desc(const void *a, const void *b)
{
    const struct task_def *ta = (const struct task_def *)a;
    const struct task_def *tb = (const struct task_def *)b;
    if (tb->util > ta->util) return 1;
    if (tb->util < ta->util) return -1;
    return 0;
}

/* Read taskset from file */
static int load_taskset(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("Failed to open taskset file");
        return -1;
    }

    num_tasks = 0;
    while (num_tasks < MAX_TASKS) {
        int C, T, D, chain, pos;
        if (fscanf(f, "%d %d %d %d %d", &C, &T, &D, &chain, &pos) != 5)
            break;
        
        tasks[num_tasks].C_ms = C;
        tasks[num_tasks].T_ms = T;
        tasks[num_tasks].D_ms = D;
        tasks[num_tasks].chain_id = chain;
        tasks[num_tasks].chain_pos = pos;
        tasks[num_tasks].util = (double)C / (double)T;
        tasks[num_tasks].cpu_id = -1;
        tasks[num_tasks].task_idx = num_tasks;
        num_tasks++;
    }

    fclose(f);
    printf("Loaded %d tasks from %s\n", num_tasks, filename);
    return 0;
}

/* First Fit Decreasing bin-packing */
static int partition_ffd(void)
{
    double cpu_util[NUM_CPUS] = {0.0, 0.0};
    
    /* Sort by utilization descending */
    qsort(tasks, num_tasks, sizeof(struct task_def), cmp_util_desc);
    
    printf("\n=== FFD Partitioning ===\n");
    printf("Tasks sorted by utilization (descending):\n");
    for (int i = 0; i < num_tasks; i++) {
        printf("  Task %d (chain %d pos %d): C=%dms T=%dms U=%.3f\n",
               tasks[i].task_idx, tasks[i].chain_id, tasks[i].chain_pos,
               tasks[i].C_ms, tasks[i].T_ms, tasks[i].util);
    }
    
    /* Assign each task to first CPU that fits */
    for (int i = 0; i < num_tasks; i++) {
        int assigned = 0;
        for (int cpu = 0; cpu < NUM_CPUS; cpu++) {
            if (cpu_util[cpu] + tasks[i].util <= 1.0) {
                tasks[i].cpu_id = cpu;
                cpu_util[cpu] += tasks[i].util;
                assigned = 1;
                break;
            }
        }
        if (!assigned) {
            printf("ERROR: Task %d (U=%.3f) cannot fit on any CPU!\n",
                   tasks[i].task_idx, tasks[i].util);
            return -1;
        }
    }
    
    /* Print assignment results */
    printf("\n=== CPU Assignment ===\n");
    for (int cpu = 0; cpu < NUM_CPUS; cpu++) {
        printf("CPU %d (total util: %.3f):\n", cpu, cpu_util[cpu]);
        for (int i = 0; i < num_tasks; i++) {
            if (tasks[i].cpu_id == cpu) {
                printf("  - Task (chain %d pos %d): C=%dms T=%dms U=%.3f\n",
                       tasks[i].chain_id, tasks[i].chain_pos,
                       tasks[i].C_ms, tasks[i].T_ms, tasks[i].util);
            }
        }
    }
    printf("\n");
    
    return 0;
}

/* Busy work to consume CPU time */
static void do_work(int work_ms)
{
    struct timespec start, now;
    long long elapsed_ns;
    long long target_ns = (long long)work_ms * 1000000LL;
    volatile double x = 1.0;
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    while (1) {
        /* Do some computation */
        for (int i = 0; i < 1000; i++) {
            x = x * 1.0001;
            if (x > 1e10) x = 1.0;
        }
        
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ns = (now.tv_sec - start.tv_sec) * 1000000000LL +
                     (now.tv_nsec - start.tv_nsec);
        
        if (elapsed_ns >= target_ns)
            break;
    }
}

/* Task thread function */
static void *task_thread(void *arg)
{
    struct task_runtime *rt = (struct task_runtime *)arg;
    struct task_def *def = rt->def;
    cpu_set_t cpuset;
    struct timespec C, T, D;
    int ret;
    
    rt->tid = syscall(SYS_gettid);
    
    /* Set CPU affinity */
    CPU_ZERO(&cpuset);
    CPU_SET(def->cpu_id, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
        perror("sched_setaffinity");
    }
    
    /* Convert ms to timespec */
    C.tv_sec = def->C_ms / 1000;
    C.tv_nsec = (def->C_ms % 1000) * 1000000L;
    T.tv_sec = def->T_ms / 1000;
    T.tv_nsec = (def->T_ms % 1000) * 1000000L;
    D.tv_sec = def->D_ms / 1000;
    D.tv_nsec = (def->D_ms % 1000) * 1000000L;
    
    /* Register with EDF scheduler */
    ret = set_edf_task(rt->tid, &C, &T, &D, 
                       def->cpu_id, def->chain_id, def->chain_pos);
    if (ret < 0) {
        printf("[Task chain%d pos%d] set_edf_task failed: %d (errno=%d)\n",
               def->chain_id, def->chain_pos, ret, errno);
        return NULL;
    }
    
    printf("[Task chain%d pos%d] TID=%d registered on CPU%d (C=%dms T=%dms)\n",
           def->chain_id, def->chain_pos, rt->tid, def->cpu_id,
           def->C_ms, def->T_ms);
    
    rt->running = 1;
    
    /* Periodic execution loop */
    while (!global_stop) {
        /* Do work for ~80% of budget to avoid overruns */
        do_work(def->C_ms * 80 / 100);
        
        rt->jobs_completed++;
        
        /* Wait for next period */
        ret = wait_until_next_period();
        if (ret < 0) {
            /* Task may have been cancelled */
            break;
        }
    }
    
    rt->running = 0;
    return NULL;
}

int main(int argc, char *argv[])
{
    const char *taskset_file = "taskset.txt";
    time_t start_time, now;
    
    if (argc > 1)
        taskset_file = argv[1];
    
    printf("========================================\n");
    printf("  EDF Loader - Section 4.2\n");
    printf("========================================\n");
    
    /* Load and partition tasks */
    if (load_taskset(taskset_file) < 0)
        return 1;
    
    if (partition_ffd() < 0)
        return 1;
    
    /* Initialize runtime structures */
    for (int i = 0; i < num_tasks; i++) {
        task_rt[i].def = &tasks[i];
        task_rt[i].running = 0;
        task_rt[i].jobs_completed = 0;
    }
    
    printf("=== Starting Tasks ===\n");
    printf("Running for %d seconds...\n\n", RUN_TIME_SEC);
    
    /* Create all task threads */
    for (int i = 0; i < num_tasks; i++) {
        if (pthread_create(&task_rt[i].thread, NULL, task_thread, &task_rt[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    
    /* Let tasks initialize */
    sleep(1);
    
    /* Run for specified duration */
    start_time = time(NULL);
    while (1) {
        sleep(10);
        now = time(NULL);
        int elapsed = (int)(now - start_time);
        
        printf("[%3d sec] Jobs completed: ", elapsed);
        for (int i = 0; i < num_tasks; i++) {
            printf("c%dp%d:%d ", tasks[i].chain_id, tasks[i].chain_pos,
                   task_rt[i].jobs_completed);
        }
        printf("\n");
        
        if (elapsed >= RUN_TIME_SEC)
            break;
    }
    
    printf("\n=== Stopping Tasks ===\n");
    global_stop = 1;
    
    /* Cancel all reservations */
    for (int i = 0; i < num_tasks; i++) {
        if (task_rt[i].tid > 0) {
            printf("Cancelling task chain%d pos%d (TID=%d)...\n",
                   tasks[i].chain_id, tasks[i].chain_pos, task_rt[i].tid);
            cancel_rsv(task_rt[i].tid);
        }
    }
    
    /* Wait for threads to exit */
    for (int i = 0; i < num_tasks; i++) {
        pthread_join(task_rt[i].thread, NULL);
    }
    
    printf("\n=== Final Statistics ===\n");
    for (int i = 0; i < num_tasks; i++) {
        printf("Task chain%d pos%d: %d jobs completed\n",
               tasks[i].chain_id, tasks[i].chain_pos,
               task_rt[i].jobs_completed);
    }
    
    printf("\nDone! Check 'dmesg' for e2e latency statistics.\n");
    
    return 0;
}
