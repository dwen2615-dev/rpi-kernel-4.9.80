/*
 * rsv_chain.c - End-to-end chain latency tracking (Section 4.3)
 */
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <asm/div64.h>
#include "rsv_internal.h"

/* Global chain latency tracking */
struct chain_latency_info chain_latency[NUM_CHAINS];

/*
 * Initialize chain latency tracking structures
 */
void chain_latency_init(void)
{
    int i;
    for (i = 0; i < NUM_CHAINS; i++) {
        spin_lock_init(&chain_latency[i].lock);
        chain_latency[i].start_time = ktime_set(0, 0);
        chain_latency[i].start_valid = 0;
        chain_latency[i].min_latency = ktime_set(999999, 0);
        chain_latency[i].max_latency = ktime_set(0, 0);
        chain_latency[i].sum_latency = ktime_set(0, 0);
        chain_latency[i].count = 0;
        chain_latency[i].last_latency = ktime_set(0, 0);
    }
    printk(KERN_INFO "rsv: chain latency tracking initialized\n");
}

/*
 * Record chain start time (called when pos=0 task starts a job)
 */
void chain_record_start(int chain_id)
{
    struct chain_latency_info *cli;

    if (chain_id < 0 || chain_id >= NUM_CHAINS)
        return;

    cli = &chain_latency[chain_id];

    spin_lock(&cli->lock);
    cli->start_time = ktime_get();
    cli->start_valid = 1;
    spin_unlock(&cli->lock);
}

/*
 * Record chain end and calculate latency (called when pos=2 task finishes job)
 */
void chain_record_end(int chain_id)
{
    struct chain_latency_info *cli;
    ktime_t now, latency;

    if (chain_id < 0 || chain_id >= NUM_CHAINS)
        return;

    cli = &chain_latency[chain_id];

    spin_lock(&cli->lock);

    if (!cli->start_valid) {
        spin_unlock(&cli->lock);
        return;
    }

    now = ktime_get();
    latency = ktime_sub(now, cli->start_time);

    /* Update statistics */
    if (ktime_compare(latency, cli->min_latency) < 0)
        cli->min_latency = latency;

    if (ktime_compare(latency, cli->max_latency) > 0)
        cli->max_latency = latency;

    cli->sum_latency = ktime_add(cli->sum_latency, latency);
    cli->count++;
    cli->last_latency = latency;

    /* Reset for next chain instance */
    cli->start_valid = 0;

    spin_unlock(&cli->lock);
}

/*
 * Print chain statistics
 */
void chain_print_stats(int chain_id)
{
    struct chain_latency_info *cli;
    u64 min_us, max_us, sum_us, avg_us;
    u32 count;
    u64 min_ms, max_ms, avg_ms;
    u32 min_frac, max_frac, avg_frac;

    if (chain_id < 0 || chain_id >= NUM_CHAINS)
        return;

    cli = &chain_latency[chain_id];

    spin_lock(&cli->lock);

    if (cli->count == 0) {
        spin_unlock(&cli->lock);
        printk(KERN_INFO "Chain %d: No completed instances\n", chain_id);
        return;
    }

    min_us = (u64)ktime_to_us(cli->min_latency);
    max_us = (u64)ktime_to_us(cli->max_latency);
    sum_us = (u64)ktime_to_us(cli->sum_latency);
    count = cli->count;

    spin_unlock(&cli->lock);

    /* Calculate average using do_div */
    avg_us = sum_us;
    do_div(avg_us, count);

    /* Convert to ms with 2 decimal places */
    min_ms = min_us;
    min_frac = do_div(min_ms, 1000);
    min_frac = min_frac / 10;

    max_ms = max_us;
    max_frac = do_div(max_ms, 1000);
    max_frac = max_frac / 10;

    avg_ms = avg_us;
    avg_frac = do_div(avg_ms, 1000);
    avg_frac = avg_frac / 10;

    /* Print in msec as required by spec */
    printk(KERN_INFO "Maximum latency of chain %d: %llu.%02u msec\n",
           chain_id, max_ms, max_frac);
    printk(KERN_INFO "Minimum latency of chain %d: %llu.%02u msec\n",
           chain_id, min_ms, min_frac);
    printk(KERN_INFO "Average latency of chain %d: %llu.%02u msec\n",
           chain_id, avg_ms, avg_frac);
}