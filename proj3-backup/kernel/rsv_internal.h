#pragma once
#include <linux/types.h>
#include <linux/time.h>
#include <linux/hrtimer.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>

struct task_struct;

/* Per-task reservation state */
struct rsv_info {
    /* 4.2: basic reservation parameters */
    struct timespec C;        /* budget */
    struct timespec T;        /* period */
    int active;               /* 1 = active */

    /* 4.4: periodic wakeup */
    struct hrtimer timer;     /* per-reservation periodic timer */
    wait_queue_head_t wq;     /* queue for wait_until_next_period */
    spinlock_t lock;          /* protects this struct */
    int waiting;              /* set when task is blocked */
    ktime_t next_release;     /* next period boundary */

    /* 4.5: accounting */
    struct timespec accum_time;   /* execution used this period */
    ktime_t sched_in_time;        /* time task was last scheduled */
    int budget_exhausted;         /* 1 if budget for this period is used */
};

/* 4.3: RM priority assignment */
void rsv_reassign_rt_prios(void);
