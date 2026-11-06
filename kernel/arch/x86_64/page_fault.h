/*
 * x86_64 页错误处理接口。
 *
 * 用户态 #PF 由 page_fault_handler 处理；内核态 #PF 转交通用异常处理器。
 */

#ifndef __PAGE_FAULT_X86_64_H__
#define __PAGE_FAULT_X86_64_H__

#include <stdint.h>

struct interrupt_frame;

void page_fault_handler(struct interrupt_frame *frame);

/* 由 idt.c 提供，供 page_fault_handler 转交内核态缺页使用。 */
void exception_handler(struct interrupt_frame *frame);

#endif /* __PAGE_FAULT_X86_64_H__ */
