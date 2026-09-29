/*
 * tty_test.h - TTY 多路复用自检测试
 *
 * 创建测试 kthread，遍历 tty_ready() 的 6 种输入组合，验证 TTY 0
 * 通配和 TTY 6-9 bitmask 匹配逻辑。测试期间短暂修改 current_tty_id，
 * 完成后恢复为 0.
 *
 * 调用时机:boot 后期 test_run() 内，此时调度器已就绪，TTY 已初始化。
 */

#ifndef __TTY_TEST_H__
#define __TTY_TEST_H__

void tty_test_run(void);

#endif
