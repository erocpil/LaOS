#ifndef __LAOS_COLOR_H__
#define __LAOS_COLOR_H__

/*
 * LaOS framebuffer 语义色板
 *
 * limine 提供 32-bit BGRA framebuffer,draw_char 直写 fg/bg 为
 * 任意 0xRRGGBB 值，无调色板约束。
 *
 * 调用约定：
 *   - 显示路径(fb)使用本文件常量
 *   - 串口 (serial_puts) 不参与着色，保持纯文本调试通道
 *   - boot log 每条整行一个色，前缀色即正文色，避免引入转义码
 */

/* 通用语义色(monitor / 状态显示) */
#define COLOR_NORMAL    0xCCCCCC /* 浅灰：默认正文 */
#define COLOR_DIM       0x808080 /* 深灰：次要信息 / 已退出 */
#define COLOR_OK        0x4EC9B0 /* 青绿：正常 / 低负载 / RUNNING */
#define COLOR_INFO      0x569CD6 /* 蓝：信息 / READY */
#define COLOR_WARN      0xDCDCAA /* 黄：活跃 / 临界区内 / 中负载 */
#define COLOR_ALERT     0xCE9178 /* 橙：告警 / BLOCKED / 高负载 */
#define COLOR_ERROR     0xF44747 /* 红：错误 / panic */
#define COLOR_WHITE     0xFFFFFF
#define COLOR_BLACK     0x000000
#define COLOR_BG_HL     0x2D2D30 /* 暗灰：高亮整行背景 */

#define COLOR_PANIC     0xFF0000 /* 纯红:panic专用，最高优先级警示 */
#define COLOR_NOTICE    0xFFA500 /* 橙黄：警告信息 */
#define COLOR_DEBUG     0x6A6A6A /* 深灰：调试信息，低优先级 */

/* boot log 子系统语义色：同一子系统色一致 */
#define COLOR_BOOT      0xCCCCCC /* 浅灰：默认正文 */
#define COLOR_PMM       0x569CD6 /* 蓝 */
#define COLOR_VMM       0x4EC9B0 /* 青 */
#define COLOR_HEAP      0x9CDCFE /* 浅蓝 */
#define COLOR_CPU       0xC586C0 /* 紫 */
#define COLOR_SCHED     0xCE9178 /* 橙 */
#define COLOR_RCU       0xDCDCAA /* 黄 */
#define COLOR_MODULE    0x4EC9B0 /* 青 */
#define COLOR_TTY       0x808080 /* 灰 */
#define COLOR_THREAD    0xDCDCAA /* 黄 */
#define COLOR_PCI       0xC586C0 /* 紫 */

#define COLOR_LOADER    0x808080 /* 灰：和TTY同色，启动早期阶段 */
#define COLOR_IDT       0xD7BA7D /* 暗金：中断相关 */
#define COLOR_GDT       0xD7BA7D /* 暗金：和IDT同色，描述符表同类 */
#define COLOR_LAPIC     0xB5CEA8 /* 浅绿:APIC/中断控制器 */
#define COLOR_TIMER     0xB5CEA8 /* 浅绿：和LAPIC同色，时钟相关 */
#define COLOR_KEYBOARD  0x9CDCFE /* 浅蓝：和HEAP同色，外设输入 */
#define COLOR_SYSCALL   0x569CD6 /* 蓝：和PMM同色，核心子系统 */
#define COLOR_ELF       0xD16969 /* 红：加载器/可执行文件 */
#define COLOR_USERSPACE 0xD16969 /* 红：和ELF同色，用户态相关 */
#define COLOR_NET       0x6A9955 /* 深绿：网络子系统 */
#define COLOR_DRIVER    0x6A9955 /* 深绿：和NET同色，驱动相关 */
#define COLOR_FS        0xE8A87C /* 橘黄：文件系统(预留) */
#define COLOR_LOCK      0xF44747 /* 亮红:spinlock/mutex，警示同步原语 */

#endif
