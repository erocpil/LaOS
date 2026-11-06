#ifndef __BOOT_PHASE_H__
#define __BOOT_PHASE_H__

/* 描述 BSP 已完成的启动阶段。AP 上线由架构相关的同步机制协调。 */
enum boot_phase {
	BOOT_PHASE_EARLY,
	BOOT_PHASE_MEMORY_READY,
	BOOT_PHASE_BOOTSTRAP_TESTS_DONE,
	BOOT_PHASE_DEVICES_READY,
	BOOT_PHASE_RUNTIME_READY,
	BOOT_PHASE_SMP_READY,
	BOOT_PHASE_SERVICES_READY,
};

enum boot_phase boot_phase_current(void);
void boot_phase_require(enum boot_phase expected);
/* 在本地中断关闭时完成下一启动阶段；阶段必须按顺序推进。 */
void boot_phase_complete(enum boot_phase phase);

/* 已上线的 AP 在启动配置任务前等待 BSP 完成指定阶段。 */
void boot_phase_wait_for(enum boot_phase phase);

#endif
