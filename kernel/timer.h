#ifndef __TIMER_H__
#define __TIMER_H__

/*
 * timer.h - 定时器与时钟
 */

#include <stdbool.h>
#include "idt.h"
#include "atomic.h"

#define TIMER_HZ 100

extern atomic64_t timer_ticks;
extern atomic64_t g_ticks; // 时钟中断里维护的全局滴答数

void timer_handler(struct interrupt_frame *frame);

#endif
