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
#include <linux/cpumask.h>
#include <asm/div64.h>

#include "rsv_internal.h"

/* From rsv_sched.c */
extern void rsv_reassign_rt_prios(void);

/* Chain latency init flag */
static int chain_init_done = 0;

/* Helper: find task by PID (pid==0 → current) */
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

/* Periodic hrtimer callback */
static enum hrtimer_restart rsv_timer_callback(struct hrtimer *timer)
{
    struct rsv_info *ri = container_of(timer, struct rsv_info, timer);
    ktime_t period;

    spin_lock(&ri->lock);

    if (!ri->active) {
        spin_unlock(&ri->lock);
        return HRTIMER_NORESTART;
    }

    /* Reset per-period accounting */
    ri->accum_time.tv_sec  = 0;
    ri->accum_time.tv_nsec = 0;
    ri->budget_exhausted   = 0;

    /* Advance EDF timing */
    period = timespec_to_ktime(ri->T);
    ri->period_start = ktime_add(ri->period_start, period);
    ri->abs_deadline = ktime_add(ri->abs_deadline, period);

    /* Wake any waiter */
    if (ri->waiting) {
        ri->waiting = 0;
        wake_up_all(&ri->wq);
    }

    /* Arm next release */
    hrtimer_forward_now(timer, period);
    ri->next_release = hrtimer_get_expires(timer);

    spin_unlock(&ri->lock);

    return HRTIMER_RESTART;
}

/* Parameter structure for EDF task metadata */
struct edf_task_params {
    int cpu_id;
    int chain_id;
    int chain_pos;
};

/*
 * 397: set_edf_task(pid, C, T, D, params)
 * Using SYSCALL_DEFINE5 with struct wrapper for last 3 params
 */
SYSCALL_DEFINE5(set_edf_task,
                pid_t, pid,
                struct timespec __user *, uC,
                struct timespec __user *, uT,
                struct timespec __user *, uD,
                struct edf_task_params __user *, uparams)
{
    struct timespec C, T, D;
    struct edf_task_params params;
    struct task_struct *p;
    struct rsv_info *ri;
    ktime_t now, period;
    int cpu_id, chain_id, chain_pos;

    if (!uC || !uT || !uD || !uparams)
        return -EFAULT;

    if (copy_from_user(&params, uparams, sizeof(params)))
        return -EFAULT;

    cpu_id = params.cpu_id;
    chain_id = params.chain_id;
    chain_pos = params.chain_pos;

    printk(KERN_INFO "set_edf_task: pid=%d cpu=%d chain=%d pos=%d\n",
           pid, cpu_id, chain_id, chain_pos);

    /* Validate CPU and chain metadata */
    if (cpu_id < 0 || cpu_id > 1)
        return -EINVAL;
    if (chain_id < 0 || chain_id > 2)
        return -EINVAL;
    if (chain_pos < 0 || chain_pos > 2)
        return -EINVAL;

    if (copy_from_user(&C, uC, sizeof(C)) ||
        copy_from_user(&T, uT, sizeof(T)) ||
        copy_from_user(&D, uD, sizeof(D)))
        return -EFAULT;

    /* Basic timespec validation */
    if (C.tv_sec < 0 || C.tv_nsec < 0 ||
        T.tv_sec < 0 || T.tv_nsec < 0 ||
        D.tv_sec < 0 || D.tv_nsec < 0)
        return -EINVAL;

    if (C.tv_nsec >= 1000000000L ||
        T.tv_nsec >= 1000000000L ||
        D.tv_nsec >= 1000000000L)
        return -EINVAL;

    if ((C.tv_sec == 0 && C.tv_nsec == 0) ||
        (T.tv_sec == 0 && T.tv_nsec == 0) ||
        (D.tv_sec == 0 && D.tv_nsec == 0))
        return -EINVAL;

    /* C <= T */
    if (C.tv_sec > T.tv_sec ||
        (C.tv_sec == T.tv_sec && C.tv_nsec > T.tv_nsec))
        return -EINVAL;

    /* D == T per spec */
    if (D.tv_sec != T.tv_sec || D.tv_nsec != T.tv_nsec)
        return -EINVAL;

    /* Find task */
    p = lookup_task(pid);
    if (!p)
        return -ESRCH;

    /* Ensure no existing reservation */
    task_lock(p);
    if (p->rsv && p->rsv->active) {
        task_unlock(p);
        return -EBUSY;
    }
    task_unlock(p);

    /* Allocate rsv_info */
    ri = kzalloc(sizeof(*ri), GFP_KERNEL);
    if (!ri)
        return -ENOMEM;

    /* Initialize EDF reservation fields */
    ri->C = C;
    ri->T = T;
    ri->D = D;
    ri->cpu_id = cpu_id;
    ri->chain_id = chain_id;
    ri->chain_pos = chain_pos;
    ri->active = 1;

    spin_lock_init(&ri->lock);
    init_waitqueue_head(&ri->wq);
    ri->waiting = 0;
    ri->budget_exhausted = 0;
    ri->accum_time.tv_sec = 0;
    ri->accum_time.tv_nsec = 0;
    ri->sched_in_time = ktime_set(0, 0);

    /* EDF timing initialization */
    now = ktime_get();
    period = timespec_to_ktime(T);
    ri->period_start = now;
    ri->next_release = ktime_add(now, period);
    ri->abs_deadline = ktime_add(now, period);

    /* Start periodic timer */
    hrtimer_init(&ri->timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS_PINNED);
    ri->timer.function = rsv_timer_callback;
    hrtimer_start(&ri->timer, ri->next_release, HRTIMER_MODE_ABS_PINNED);

    /* Attach reservation to task */
    task_lock(p);
    p->rsv = ri;
    task_unlock(p);

    /* Pin task to specific CPU */
    set_cpus_allowed_ptr(p, cpumask_of(cpu_id));

    /* Initialize chain latency tracking (once) */
    if (!chain_init_done) {
        chain_latency_init();
        chain_init_done = 1;
    }

    /* Reassign RT priorities */
    rsv_reassign_rt_prios();

    printk(KERN_INFO "set_edf_task SUCCESS: pid=%d cpu=%d chain=%d pos=%d C=%ld.%09ld T=%ld.%09ld\n",
           pid, cpu_id, chain_id, chain_pos,
           C.tv_sec, C.tv_nsec, T.tv_sec, T.tv_nsec);

    return 0;
}

/*
 * 398: cancel_rsv(pid)
 */
SYSCALL_DEFINE1(cancel_rsv, pid_t, pid)
{
    struct task_struct *p;
    struct rsv_info *ri;
    int chain_id, chain_pos;

    p = lookup_task(pid);
    if (!p)
        return -ESRCH;

    task_lock(p);
    ri = p->rsv;
    if (!ri || !ri->active) {
        task_unlock(p);
        return -EINVAL;
    }

    /* Save chain info before freeing */
    chain_id = ri->chain_id;
    chain_pos = ri->chain_pos;

    p->rsv = NULL;
    task_unlock(p);

    spin_lock(&ri->lock);
    ri->active = 0;
    ri->waiting = 0;
    wake_up_all(&ri->wq);
    spin_unlock(&ri->lock);

    hrtimer_cancel(&ri->timer);
    kfree(ri);

    rsv_reassign_rt_prios();

    /* Print chain stats when end task (pos=2) is cancelled */
    if (chain_pos == 2) {
        chain_print_stats(chain_id);
    }

    printk(KERN_INFO "cancel_rsv: pid=%d\n", pid);
    return 0;
}

/*
 * 399: wait_until_next_period()
 */
SYSCALL_DEFINE0(wait_until_next_period)
{
    struct rsv_info *ri = current->rsv;
    int ret;
    int chain_id, chain_pos;

    if (!ri || !ri->active)
        return -EINVAL;

    chain_id = ri->chain_id;
    chain_pos = ri->chain_pos;

    /* If end task (pos=2), record chain end before sleeping */
    if (chain_pos == 2)
        chain_record_end(chain_id);

    spin_lock(&ri->lock);
    if (!ri->active) {
        spin_unlock(&ri->lock);
        return -EINVAL;
    }
    ri->waiting = 1;
    spin_unlock(&ri->lock);

    ret = wait_event_interruptible(ri->wq,
                                   !ri->active || !ri->waiting);

    if (!ri->active)
        return -EINVAL;

    if (ret == -ERESTARTSYS)
        return -EINTR;

    /* If start task (pos=0), record chain start after waking */
    if (chain_pos == 0)
        chain_record_start(chain_id);

    rsv_reassign_rt_prios();

    return 0;
}

/*
 * 400: get_e2e_latency(chain_id, latency_buf)
 * Returns most recent e2e latency for a chain
 */
SYSCALL_DEFINE2(get_e2e_latency, int, chain_id, struct timespec __user *, latency_buf)
{
    struct chain_latency_info *cli;
    struct timespec ts;
    s64 ns;
    u64 ns_abs;
    u32 billion = 1000000000UL;

    if (chain_id < 0 || chain_id >= NUM_CHAINS)
        return -EINVAL;

    if (!latency_buf)
        return -EFAULT;

    cli = &chain_latency[chain_id];

    spin_lock(&cli->lock);
    ns = ktime_to_ns(cli->last_latency);
    spin_unlock(&cli->lock);

    /* Use do_div for 64-bit division on ARM */
    ns_abs = (u64)ns;
    ts.tv_nsec = do_div(ns_abs, billion);
    ts.tv_sec = (long)ns_abs;

    if (copy_to_user(latency_buf, &ts, sizeof(ts)))
        return -EFAULT;

    return 0;
}