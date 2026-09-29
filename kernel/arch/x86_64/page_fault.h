/*
 * page_fault.h — x86_64 页错误恢复
 *
 * 拦截用户态 #PF (vector 14)，按需分页或杀死进程。
 * 内核态 #PF 仍走 exception_handler → panic。
 */

#ifndef __PAGE_FAULT_X86_64_H__
#define __PAGE_FAULT_X86_64_H__

#include <stdint.h>

struct interrupt_frame;

void page_fault_handler(struct interrupt_frame *frame);

/* Declared in idt.c — called for kernel-mode #PF */
void exception_handler(struct interrupt_frame *frame);

#endif /* __PAGE_FAULT_X86_64_H__ */
