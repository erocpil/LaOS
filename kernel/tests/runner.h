#ifndef __KERNEL_TESTS_RUNNER_H__
#define __KERNEL_TESTS_RUNNER_H__

#include "boot_info.h"

/* 在 IRQ 关闭且设备尚未注册时执行启动自测；失败会中止启动。
 * 阶段从 MEMORY_READY 推进到 BOOTSTRAP_TESTS_DONE。 */
void kernel_tests_run_bootstrap(void);

/* 要求 SMP_READY。早期单元测试报告缓存结果，其余测试按常规流程加载、
 * 准备并启动；调用方随后完成 SERVICES_READY 阶段。 */
struct task_conf;
int kernel_tests_start_config(struct task_conf *conf);
void kernel_tests_start(const struct boot_module_list *modules);

#endif
