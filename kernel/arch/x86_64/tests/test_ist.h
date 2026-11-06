#ifndef __TEST_IST_H__
#define __TEST_IST_H__

/*
 * ist_test_start - 启动 IST 中断栈隔离自检
 *
 * 仅在 CONFIG_IST_TEST=1 时提供测试实现；否则该接口为空操作。
 * 应在启动完成、时钟中断启用且所有处理器上线后调用，通常位于进入 idle 前。
 *
 * 测试目标：验证 double-fault (#DF) 走 IST_SLOT_DF 独立栈而非当前线程栈。
 *
 * 成功时，串口或 TTY9 输出捕获处理器及 IST 栈信息，随后进入 panic。
 * 若 IST 未生效，双重故障可能升级为三重故障并导致 QEMU 重启；此时日志
 * 通常停留在测试线程启动之前。
 */
void ist_test_start(void);

#endif
