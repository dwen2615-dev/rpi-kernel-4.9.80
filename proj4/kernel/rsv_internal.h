#pragma once
#include <linux/types.h>
#include <linux/time.h>
#include <linux/hrtimer.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>

struct task_struct;

/*
 * Number of chains supported (0, 1, 2)
 */
#define NUM_CHAINS 3

/*
 * Per-chain end-to-end latency tracking (Section 4.3)
 * This is global, not per-task, since chains span multiple tasks.
 */
struct chain_latency_info {
    spinlock_t lock;
    ktime_t start_time;       /* When pos=0 task started current job */
    int start_valid;          /* 1 if start_time is valid */
    
    /* Statistics */
    ktime_t min_latency;
    ktime_t max_latency;
    ktime_t sum_latency;
    int count;                /* Number of completed chain instances */
    ktime_t last_latency;     /* Most recent e2e latency (for syscall 400) */
};

/*
 * Per-task reservation / EDF state.
 *
 * Project 3 fields are kept (C, T, active, timer, wq, lock, waiting,
 * next_release, accum_time, sched_in_time, budget_exhausted).
 *
 * Project 4 (4.1) adds:
 *   - D        : relative deadline (D == T per spec)
 *   - cpu_id   : which CPU this task is pinned to
 *   - chain_id : processing chain ID
 *   - chain_pos: position within the chain
 *   - period_start, abs_deadline: EDF timing for current job
 */
struct rsv_info {
    /* 4.1 / 4.2: basic reservation parameters + EDF metadata */
    struct timespec C;        /* budget */
    struct timespec T;        /* period */
    struct timespec D;        /* relative deadline (D == T) */
    int cpu_id;               /* partitioned EDF: assigned CPU */
    int chain_id;             /* chain index (0,1,2) */
    int chain_pos;            /* position in chain (0,1,2) */
    int active;               /* 1 = active reservation */

    /* 4.4 (Proj 3): periodic wakeup */
    struct hrtimer timer;     /* per-reservation periodic timer */
    wait_queue_head_t wq;     /* queue for wait_until_next_period */
    spinlock_t lock;          /* protects this struct */
    int waiting;              /* set when task is blocked */
    ktime_t next_release;     /* next period boundary */

    /* 4.5 (Proj 3): CPU-time accounting within each period */
    struct timespec accum_time;   /* execution used this period */
    ktime_t sched_in_time;        /* time task was last scheduled IN */
    int budget_exhausted;         /* 1 if budget for this period is used */

    /* EDF timing for current job (Project 4) */
    ktime_t period_start;     /* start time of current period/job */
    ktime_t abs_deadline;     /* absolute deadline for current job */
};

/* Global chain latency tracking array */
extern struct chain_latency_info chain_latency[NUM_CHAINS];

/* Initialize chain latency tracking (call once at boot or module init) */
void chain_latency_init(void);

/* Record chain start (called when pos=0 task starts job) */
void chain_record_start(int chain_id);

/* Record chain end (called when pos=2 task finishes job) */
void chain_record_end(int chain_id);

/* Print chain statistics (called from cancel_rsv when appropriate) */
void chain_print_stats(int chain_id);

/* Project 3: RM priority assignment (still referenced in existing code) */
void rsv_reassign_rt_prios(void);

/* Cleanup hook used from do_exit() */
void rsv_cleanup_task(struct task_struct *p);












