#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/math64.h>
#include <linux/signal.h>

#include "rsv_internal.h"

/*
 * 4.5: runtime accounting + budget enforcement
 *
 * These functions are called from the scheduler when a task is
 * switched out and in. They only do work for tasks with an active
 * reservation (p->rsv && p->rsv->active).
 */

/* Helper: compare timespec a >= b ? */
static inline int timespec_ge(const struct timespec *a,
                              const struct timespec *b)
{
    if (a->tv_sec > b->tv_sec)
        return 1;
    if (a->tv_sec < b->tv_sec)
        return 0;
    return a->tv_nsec >= b->tv_nsec;
}

/* Called on context switch OUT of p */
void rsv_on_switch_out(struct task_struct *p)
{
    struct rsv_info *ri;
    ktime_t now, delta;
    struct timespec delta_ts;
    long sec, nsec;

    if (!p)
        return;

    ri = p->rsv;
    if (!ri || !ri->active)
        return;

    /* If we never recorded sched_in_time, nothing to do */
    if (ktime_to_ns(ri->sched_in_time) == 0)
        return;

    now = ktime_get();
    delta = ktime_sub(now, ri->sched_in_time);
    if (ktime_to_ns(delta) <= 0)
        return;

    delta_ts = ktime_to_timespec(delta);

    spin_lock(&ri->lock);

    /* accum_time += delta_ts */
    sec  = ri->accum_time.tv_sec  + delta_ts.tv_sec;
    nsec = ri->accum_time.tv_nsec + delta_ts.tv_nsec;
    if (nsec >= 1000000000L) {
        sec++;
        nsec -= 1000000000L;
    }
    ri->accum_time.tv_sec  = sec;
    ri->accum_time.tv_nsec = nsec;

    /*
     * If budget is already exhausted, nothing more to do here.
     * (We only log once per period.)
     */
    if (!ri->budget_exhausted) {
        if (timespec_ge(&ri->accum_time, &ri->C)) {
            long long used_ns, C_ns;
            long util_percent = 0;

            ri->budget_exhausted = 1;

            /* Compute utilization (%) for logging */
            used_ns = (long long)ri->accum_time.tv_sec * 1000000000LL +
                      (long long)ri->accum_time.tv_nsec;
            C_ns    = (long long)ri->C.tv_sec * 1000000000LL +
                      (long long)ri->C.tv_nsec;

            if (C_ns > 0) {
                unsigned long long temp = used_ns * 100;
                do_div(temp, C_ns);
                util_percent = (long)temp;
            }

            printk(KERN_INFO "Task %d: budget overrun (util: %ld %%)\n",
                   p->pid, util_percent);

            //  send_sig(SIGUSR1, p, 0);

            /*
             * DO NOT call sched_setscheduler_nocheck() here!
             * We're in scheduler context with locks held.
             * The demotion will happen in rsv_reassign_rt_prios()
             * which checks budget_exhausted flag.
             */
        }
    }

    spin_unlock(&ri->lock);
}

/* Called on context switch IN to p */
void rsv_on_switch_in(struct task_struct *p)
{
    struct rsv_info *ri;

    if (!p)
        return;

    ri = p->rsv;
    if (!ri || !ri->active)
        return;

    ri->sched_in_time = ktime_get();
}
