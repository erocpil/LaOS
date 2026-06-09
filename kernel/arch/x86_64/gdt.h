#ifndef __GDT_H__
#define __GDT_H__

/*
 * gdt.h - GDT/TSS 结构定义与入口选择子常量
 */

#include <stdint.h>

#define GDT_SIZE 10

#define USER_CS 0x23
#define USER_SS 0x1b

// GDT 条目结构
struct gdt_entry {
	uint16_t limit_low;
	uint16_t base_low;
	uint8_t  base_mid;
	uint8_t  access;
	uint8_t  granularity;
	uint8_t  base_high;
} __attribute__((packed));

// GDTR 指针结构
struct gdt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

// TSS 结构体定义 (x86_64)
struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;      // 当从用户态进入内核态(Ring 0)时，自动切换到这个栈指针
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];    // 中断栈表 (IST)，用于处理双重错误等特殊情况
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

// 扩展 GDT 描述符结构，以支持 16 字节的系统段描述符
struct gdt_system_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  limit_high;
    uint8_t  base_high;
    uint32_t base_upper32;
    uint32_t reserved;
} __attribute__((packed));

// extern struct gdt_entry per_cpu_gdt[MAX_CPUS][GDT_SIZE];
// extern struct gdt_ptr per_cpu_gdt_ptr[MAX_CPUS];
// extern struct tss_entry per_cpu_tss[MAX_CPUS];

void gdt_init_cpu(int cpu_id, uint64_t kernel_stack);
void gdt_init_dynamic(uint32_t n);
void gdt_reload(struct gdt_ptr *ptr);
void tss_load(void);
void gdt_init(uint32_t cpu_id, uint64_t kernel_stack);
void tss_set_rsp0(uint32_t cpu_id, uint64_t rsp0);
uint64_t get_per_cpu_tss_rsp0(uint32_t cpu);

#endif
