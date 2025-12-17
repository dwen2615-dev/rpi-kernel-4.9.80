#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>   /* <-- NEW: for in_interrupt() */
#include "rsv_internal.h"

#define RSV_MAX_RT_PRIO     50
#define RSV_MAX_RSV_TASKS   64   /* enough for our lab */

/*
 * Recompute RT priorities of all tasks that currently have an active
 * reservation, using Rate Monotonic (shorter T => higher priority).
 *
 *  - If rsv->budget_exhausted == 1, keep task in SCHED_NORMAL for the
 *    remainder of the current period.
 *  - Otherwise, assign an RT priority via RM (shorter T => higher prio).
 */
void rsv_reassign_rt_prios(void)
{
    struct task_struct *p;
    struct task_struct *tasks[RSV_MAX_RSV_TASKS];
    struct timespec periods[RSV_MAX_RSV_TASKS];
    int rank[RSV_MAX_RSV_TASKS];
    int n = 0;
    int i, j;

    /*
     * IMPORTANT:
     * Never run this from interrupt / hrtimer / scheduler-internal
     * context — sched_setscheduler_nocheck() can sleep and will
     * deadlock if called there.
     *
     * If someone accidentally calls us from such a context (e.g., from
     * the hrtimer callback or a context-switch hook), just skip the
     * reprioritization instead of hanging the system.
     */
    if (in_interrupt()) {
        pr_warn("rsv: rsv_reassign_rt_prios called in interrupt context, skipping\n");
        return;
    }

    /* 1) Collect all reserved tasks, holding a reference on each */
    read_lock(&tasklist_lock);
    for_each_process(p) {
        if (p->rsv && p->rsv->active) {
            if (n >= RSV_MAX_RSV_TASKS)
                break;

            /* Make sure task_struct can't be freed while we use it */
            get_task_struct(p);

            tasks[n]   = p;
            periods[n] = p->rsv->T;
            n++;
        }
    }
    read_unlock(&tasklist_lock);

    if (n == 0)
        return;

    /* 2) Rank tasks by period (RM): rank = # with strictly shorter T */
    for (i = 0; i < n; i++) {
        rank[i] = 0;
        for (j = 0; j < n; j++) {
            if (timespec_compare(&periods[j], &periods[i]) < 0)
                rank[i]++;
        }
    }

    /* 3) Assign priorities based on rank and budget state */
    for (i = 0; i < n; i++) {
        struct sched_param sp;
        int prio;

        /* Skip if reservation disappeared while we were ranking */
        if (!tasks[i]->rsv || !tasks[i]->rsv->active) {
            put_task_struct(tasks[i]);
            continue;
        }

        /* If budget exhausted, keep task in SCHED_NORMAL for this period */
        if (tasks[i]->rsv->budget_exhausted) {
            sp.sched_priority = 0;
            sched_setscheduler_nocheck(tasks[i], SCHED_NORMAL, &sp);
            printk(KERN_INFO,
                   "rsv: pid=%d keeping SCHED_NORMAL (budget exhausted)\n",
                   tasks[i]->pid);
        } else {
            /* Apply RM-based RT priority */
            prio = RSV_MAX_RT_PRIO - rank[i];
            if (prio < 1)
                prio = 1;

            sp.sched_priority = prio;
            sched_setscheduler_nocheck(tasks[i], SCHED_FIFO, &sp);
            printk(KERN_INFO,
                   "rsv: pid=%d T=%ld.%09ld -> RT prio=%d\n",
                   tasks[i]->pid,
                   (long)periods[i].tv_sec,
                   periods[i].tv_nsec,
                   prio);
        }

        /* Drop our reference now that we're done with this task */
        put_task_struct(tasks[i]);
    }
}

/*
 * Called from do_exit() to clean up a task's reservation
 * when it dies without calling cancel_rsv().
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

    /* Cancel timer (4.4+) */
    hrtimer_cancel(&ri->timer);

    kfree(ri);
    p->rsv = NULL;

    /* Task disappeared -> RM priorities among survivors may change */
    rsv_reassign_rt_prios();
}
