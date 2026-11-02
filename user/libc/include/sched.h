#ifndef _SCHED_H
#define _SCHED_H
#include <sys/types.h>
#include <uapi/caps.h>
/* SCHED_OTHER / SCHED_FIFO si struct sched_param vin din <uapi/caps.h>. SCHED_FIFO (prioritate 1..99) cere CAP_RT.
 * Doar pentru procesul curent (pid 0 sau getpid()) */
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
int sched_getscheduler(pid_t pid);
int sched_yield(void);
#endif
