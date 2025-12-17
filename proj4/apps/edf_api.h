#ifndef EDF_API_H
#define EDF_API_H

#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>

/* Must match kernel syscall numbers */
#define SYS_SET_EDF_TASK            397
#define SYS_CANCEL_RSV              398
#define SYS_WAIT_UNTIL_NEXT_PERIOD  399

/* Must match kernel struct edf_task_params */
struct edf_task_params {
    int cpu_id;
    int chain_id;
    int chain_pos;
};

static inline int set_edf_task(pid_t pid,
                               struct timespec *C,
                               struct timespec *T,
                               struct timespec *D,
                               int cpu_id,
                               int chain_id,
                               int chain_pos)
{
    struct edf_task_params p = { cpu_id, chain_id, chain_pos };
    return syscall(SYS_SET_EDF_TASK, pid, C, T, D, &p);
}

static inline int cancel_rsv(pid_t pid)
{
    return syscall(SYS_CANCEL_RSV, pid);
}

static inline int wait_until_next_period(void)
{
    return syscall(SYS_WAIT_UNTIL_NEXT_PERIOD);
}

#endif /* EDF_API_H */
