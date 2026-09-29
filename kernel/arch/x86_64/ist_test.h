#ifndef __IST_TEST_H__
#define __IST_TEST_H__

/**
 * ist_test_start - 启动 IST 中断栈隔离自检
 *
 * 仅在 CONFIG_IST_TEST=1 时有实体，否则为空 stub.
 * 期望调用位置:boot 完成，时钟中断已启动，SMP 已 up 之后(通常在 idle 之前).
 *
 * 测试目标：验证 double-fault (#DF) 走 IST_SLOT_DF 独立栈而非当前线程栈。
 *
 * 通过：串口/TTY9 打印 "[ist-test] DF caught on IST stack cpu=X" 后进入 panic.
 * 失败(未启用 IST):QEMU 直接重启(triple-fault)，日志停在测试线程启动前。
 */
void ist_test_start(void);

#endif
