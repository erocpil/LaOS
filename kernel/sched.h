#ifndef __SCHED_H__
#define __SCHED_H__

/*
 * sched.h - 调度器与同步原语接口
 */

#include "stdint.h"

void schedule(void);
int schedule_timeout(uint64_t t);
void __schedule_irq(void);
void __schedule_preempt(void);
void sched_snap(void);
int check_need_schedule(void);

#endif
