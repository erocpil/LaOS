#ifndef __LAPIC_H__
#define __LAPIC_H__

/*
 * lapic.h - LAPIC/IOAPIC MMIO 偏移，寄存器，函数声明
 */

#include <stdint.h>

#define LAPIC_BASE_PHYS 0xFEE00000
// LAPIC 寄存器偏移
#define LAPIC_REG_ID          0x0020
#define LAPIC_REG_EOI         0x00B0
#define LAPIC_REG_SPURIOUS    0x00F0
#define LAPIC_REG_ICR_LOW     0x0300
#define LAPIC_REG_ICR_HIGH    0x0310
#define LAPIC_REG_LVT_TIMER   0x0320
#define LAPIC_REG_TIMER_INIT  0x0380
#define LAPIC_REG_TIMER_CURR  0x0390
#define LAPIC_REG_TIMER_DIV   0x03E0

// IOAPIC 默认物理地址
#define IOAPIC_BASE_PHYS 0xFEC00000

// 两个映射寄存器(相对偏移)
#define IOREGSEL 0x00  // 索引选择寄存器
#define IOWIN    0x10  // 数据寄存器

#define MMIO_PAGES 1 // LAPIC 寄存器空间通常只需 1 个页 (4KB)

uint32_t ioapic_read(uint8_t reg);
void ioapic_write(uint8_t reg, uint32_t data);
void lapic_eoi(void);
void lapic_init(void);
void ioapic_init(void);
uint32_t get_cpu_id(void);
uint32_t get_cpu_id_cpuid(void);
void lapic_map(uint64_t *pml4);

#endif
