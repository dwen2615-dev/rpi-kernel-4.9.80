#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/list.h>

#include "rsv_internal.h"

#define RSV_MAX_RT_PRIO 99  /* Highest RT priority */
#define RSV_MAX_RSV_TASKS 64

/*
 * EDF Priority Assignment (Project 4)
 * 
 * For partitioned EDF, each CPU maintains its own priority ordering.
 * The task with the EARLIEST absolute deadline gets the HIGHEST priority.
 * 
 * NOTE: This is different from Project 3's Rate Monotonic (RM) which used
 * period T for static priorities. EDF priorities are DYNAMIC and must be
 * recalculated whenever deadlines change (i.e., at the start of each period).
 */

/*
 * Helper: Compare two ktime_t values
 * Returns: <0 if a < b, 0 if a == b, >0 if a > b
 */
static inline int ktime_compare_safe(ktime_t a, ktime_t b)
{
    if (ktime_to_ns(a) < ktime_to_ns(b))
        return -1;
    if (ktime_to_ns(a) > ktime_to_ns(b))
        return 1;
    return 0;
}

/*
 * Reassign RT priorities for all tasks with active reservations
 * using EDF (Earliest Deadline First) policy.
 * 
 * EDF Rule: Task with earliest absolute deadline gets highest priority.
 * 
 * Since we're doing PARTITIONED EDF, we should ideally assign priorities
 * per-CPU, but for simplicity in 4.1, we'll do global assignment across
 * all reserved tasks. We'll optimize this later if needed.
 */
void rsv_reassign_rt_prios(void) {
    struct task_struct *p, *g;  /* ✅ declare BOTH at the top */
    struct task_struct *tasks[RSV_MAX_RSV_TASKS];
    ktime_t deadlines[RSV_MAX_RSV_TASKS];
    int rank[RSV_MAX_RSV_TASKS];
    int n = 0;
    int i, j;

    printk(KERN_INFO "rsv_reassign_rt_prios: CALLED\n");

    if (in_interrupt()) {
        pr_warn("rsv: rsv_reassign_rt_prios called in interrupt context, skipping\n");
        return;
    }

    /* 1) Collect all reserved tasks and their absolute deadlines */
    read_lock(&tasklist_lock);
    for_each_process_thread(g, p) {   /* g = group leader, p = each thread */
        if (p->rsv && p->rsv->active) {
            if (n >= RSV_MAX_RSV_TASKS)
                break;

            get_task_struct(p);
            tasks[n]     = p;
            deadlines[n] = p->rsv->abs_deadline;
            n++;
        }
    }
    read_unlock(&tasklist_lock);

    if (n == 0)
        return;

    /* ... keep your ranking + priority assignment code unchanged ... */

    if (n == 0)
        return;

    /*
     * 2) Rank tasks by absolute deadline (EDF)
     * rank[i] = number of tasks with EARLIER deadline than task i
     * 
     * This is different from RM which ranked by period T.
     */
    for (i = 0; i < n; i++) {
        rank[i] = 0;
        for (j = 0; j < n; j++) {
            if (ktime_compare_safe(deadlines[j], deadlines[i]) < 0) {
                rank[i]++;  /* Task j has earlier deadline */
            }
        }
    }

    /*
     * 3) Assign RT priorities based on EDF ranking
     * Earliest deadline → Highest priority (99)
     * Latest deadline → Lowest priority
     */
    for (i = 0; i < n; i++) {
        struct sched_param sp;
        int prio;

        /* Skip if reservation disappeared */
        if (!tasks[i]->rsv || !tasks[i]->rsv->active) {
            put_task_struct(tasks[i]);
            continue;
        }

        /* If budget exhausted, demote to SCHED_NORMAL for rest of period */
        if (tasks[i]->rsv->budget_exhausted) {
            sp.sched_priority = 0;
            sched_setscheduler_nocheck(tasks[i], SCHED_NORMAL, &sp);
            
            printk(KERN_INFO "rsv: pid=%d demoted to SCHED_NORMAL (budget exhausted)\n",
                   tasks[i]->pid);
        } else {
            /*
             * EDF priority assignment:
             * Priority = RSV_MAX_RT_PRIO - rank
             * 
             * Example with 3 tasks:
             * - Task with earliest deadline: rank=0 → priority=99
             * - Task with middle deadline:   rank=1 → priority=98
             * - Task with latest deadline:   rank=2 → priority=97
             */
            prio = RSV_MAX_RT_PRIO - rank[i];
            if (prio < 1)
                prio = 1;  /* Clamp to valid RT range [1, 99] */

            sp.sched_priority = prio;
            sched_setscheduler_nocheck(tasks[i], SCHED_FIFO, &sp);

            printk(KERN_INFO "rsv: pid=%d cpu=%d deadline=%lld ns → RT prio=%d (EDF)\n",
                   tasks[i]->pid,
                   tasks[i]->rsv->cpu_id,
                   ktime_to_ns(deadlines[i]),
                   prio);
        }

        put_task_struct(tasks[i]);
    }
}

/*
 * Cleanup reservation when task exits without calling cancel_rsv
 * (Called from do_exit() hook)
 */
void rsv_cleanup_task(struct task_struct *p)
{
    struct rsv_info *ri;

    if (!p)
        return;

    ri = p->rsv;
    if (!ri)
        return;

    printk(KERN_INFO "rsv_cleanup: freeing reservation for pid=%d\n", p->pid);

    ri->active = 0;
    hrtimer_cancel(&ri->timer);
    kfree(ri);
    p->rsv = NULL;

    /* Recompute priorities for remaining tasks */
    rsv_reassign_rt_prios();
}
