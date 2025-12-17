#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/spinlock.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/wait.h>

#include "rsv_internal.h"

/* From rsv_sched.c (RM priority assignment) */
extern void rsv_reassign_rt_prios(void);

/*
 * Helper: find task by PID
 */
static inline struct task_struct *lookup_task(pid_t pid)
{
    struct task_struct *p;

    if (pid == 0)
        return current;

    rcu_read_lock();
    p = pid_task(find_vpid(pid), PIDTYPE_PID);
    rcu_read_unlock();

    return p;
}

/*
 * hrtimer callback for periodic release (4.4)
 * Wakes any task blocked in wait_until_next_period()
 * and schedules the next expiry.
 */
static enum hrtimer_restart rsv_timer_callback(struct hrtimer *timer)
{
    struct rsv_info *ri = container_of(timer, struct rsv_info, timer);
    ktime_t period;

    /* Protect against races with cancel_rsv / cleanup */
    spin_lock(&ri->lock);

    if (!ri->active) {
        spin_unlock(&ri->lock);
        return HRTIMER_NORESTART;
    }

    /* Reset accumulator for new period (4.5) */
    ri->accum_time.tv_sec = 0;
    ri->accum_time.tv_nsec = 0;
    ri->budget_exhausted = 0;

    /* Wake any waiter */
    if (ri->waiting) {
        ri->waiting = 0;
        wake_up_all(&ri->wq);
    }

    /* Schedule next period */
    period = timespec_to_ktime(ri->T);
    hrtimer_forward_now(timer, period);
    ri->next_release = hrtimer_get_expires(timer);

    spin_unlock(&ri->lock);

    return HRTIMER_RESTART;
}

/*
 * 397: set_rsv(pid, C, T)
 */
SYSCALL_DEFINE3(set_rsv, pid_t, pid,
                struct timespec __user *, uC,
                struct timespec __user *, uT)
{
    struct timespec C, T;
    struct task_struct *p;
    struct rsv_info *ri;
    ktime_t now, period;

    /* Validate user pointers */
    if (!uC || !uT)
        return -EFAULT;

    /* Copy from user space */
    if (copy_from_user(&C, uC, sizeof(C)))
        return -EFAULT;

    if (copy_from_user(&T, uT, sizeof(T)))
        return -EFAULT;

    /* Validate timespec values */
    if (C.tv_sec < 0 || C.tv_nsec < 0 ||
        T.tv_sec < 0 || T.tv_nsec < 0)
        return -EINVAL;

    if (C.tv_nsec >= 1000000000L || T.tv_nsec >= 1000000000L)
        return -EINVAL;

    if ((C.tv_sec == 0 && C.tv_nsec == 0) ||
        (T.tv_sec == 0 && T.tv_nsec == 0))
        return -EINVAL;

    /* Check C <= T */
    if (C.tv_sec > T.tv_sec ||
        (C.tv_sec == T.tv_sec && C.tv_nsec > T.tv_nsec))
        return -EINVAL;

    /* Find task */
    p = lookup_task(pid);
    if (!p)
        return -ESRCH;

    /* Check for existing reservation (with task_lock protection) */
    task_lock(p);
    if (p->rsv && p->rsv->active) {
        task_unlock(p);
        return -EBUSY;
    }
    task_unlock(p);

    /* Allocate reservation struct */
    ri = kzalloc(sizeof(*ri), GFP_KERNEL);
    if (!ri)
        return -ENOMEM;

    /* Initialize reservation fields */
    ri->C      = C;
    ri->T      = T;
    ri->active = 1;

    spin_lock_init(&ri->lock);
    init_waitqueue_head(&ri->wq);
    ri->waiting = 0;
    ri->budget_exhausted = 0;

    ri->accum_time.tv_sec = 0;
    ri->accum_time.tv_nsec = 0;
    ri->sched_in_time = ktime_set(0, 0);

    /* 4.4: initialize and start periodic hrtimer */
    period = timespec_to_ktime(T);
    now    = ktime_get();
    ri->next_release = ktime_add(now, period);

    hrtimer_init(&ri->timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS_PINNED);
    ri->timer.function = rsv_timer_callback;
    hrtimer_start(&ri->timer, ri->next_release, HRTIMER_MODE_ABS_PINNED);

    /* Attach to task */
    task_lock(p);
    p->rsv = ri;
    task_unlock(p);

    /* 4.3: Reassign RT priorities */
    rsv_reassign_rt_prios();

    printk(KERN_INFO "set_rsv: pid=%d C=%ld.%09ld T=%ld.%09ld\n",
           pid, C.tv_sec, C.tv_nsec, T.tv_sec, T.tv_nsec);

    return 0;
}

/*
 * 398: cancel_rsv(pid)
 */
SYSCALL_DEFINE1(cancel_rsv, pid_t, pid)
{
    struct task_struct *p;
    struct rsv_info *ri;

    /* Find task */
    p = lookup_task(pid);
    if (!p)
        return -ESRCH;

    /* Get and validate reservation with protection */
    task_lock(p);
    ri = p->rsv;
    if (!ri || !ri->active) {
        task_unlock(p);
        return -EINVAL;
    }

    /* Detach from task first */
    p->rsv = NULL;
    task_unlock(p);

    /* Mark inactive under ri->lock so callback/waiters see it atomically */
    spin_lock(&ri->lock);
    ri->active  = 0;
    ri->waiting = 0;
    spin_unlock(&ri->lock);

    /* 4.4: Stop timer and wait for any running callback */
    hrtimer_cancel(&ri->timer);

    /* Free memory */
    kfree(ri);

    /* 4.3: Reassign RT priorities */
    rsv_reassign_rt_prios();

    printk(KERN_INFO "cancel_rsv: pid=%d\n", pid);
    return 0;
}

/*
 * 399: wait_until_next_period()
 * Block the current task until its next release time.
 */
SYSCALL_DEFINE0(wait_until_next_period)
{
    struct rsv_info *ri = current->rsv;
    int ret;

    if (!ri || !ri->active)
        return -EINVAL;

    /* Mark that this task is waiting for the next period */
    spin_lock(&ri->lock);
    if (!ri->active) {
        spin_unlock(&ri->lock);
        return -EINVAL;
    }
    ri->waiting = 1;
    spin_unlock(&ri->lock);

    /*
     * Sleep until the timer clears waiting or reservation is deactivated.
     * This handles the race where the timer fires before we actually sleep:
     * wait_event_* reevaluates the condition before blocking.
     */
    ret = wait_event_interruptible(
        ri->wq,
        (!ri->active) || (!ri->waiting)
    );

    /* If reservation was cancelled while we were sleeping */
    if (!ri->active)
        return -EINVAL;

    if (ret == -ERESTARTSYS)
        return -EINTR;

     /*
     * New period has started, and budget_exhausted was reset
     * by the timer callback. Now, in process context, we can
     * safely recompute priorities for all reserved tasks.
     */
    rsv_reassign_rt_prios();

    return 0;
}
