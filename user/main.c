/* user/main.c — 用户程序（两架构通用）
 *
 * 功能：验证 ELF 加载 → EL0 入口 → SVC write/sleep/exit 完整链路。
 * 循环打印 + msleep 验证 SYS_SLEEP 和调度器交互。
 * P4-4: 页错误恢复测试（MAP_LAZY + 按需分页）— 当前仅 ARM64 支持。
 */

#include <stdint.h>
#include <stddef.h>

#include "user.lib.h"

/* 系统调用包装（user.lib_arm64.c / user.lib.c 提供） */
int  write(int fd, const void *buf, unsigned long count);
void exit(int status) __attribute__((noreturn));
void yield(void);

size_t strlen(const char *s)
{
    size_t n = 0;

    if (!s)
        return 0;

    while (*s != '\0') {
        n++;
        s++;
    }

    return n;
}

/* 基于 write 的简易 kprintf */
static void uprint(const char *msg)
{
    write(1, msg, strlen(msg) + 1);
}

int main(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    uprint("LaOS user ELF: loaded OK\n");
    yield();  /* 测试 SYS_YIELD */

    uint32_t loops = 0;
    while (loops++ < 2u) {
        uprint("LaOS user ELF: running ...\n");
        msleep(5000);
    }

	/*
	 * P4-4 页错误恢复测试：
	 *
	 * Test 1: MAP_LAZY 分配 2 页 — 只创建 VMA，不分配物理页。
	 *         写入触发 #PF / Data Abort → Translation fault → 按需分页 → 成功。
	 */
	char *buf = (char *)mmap(8192, PROT_READ | PROT_WRITE, MAP_LAZY);
	if (!buf) {
		uprint("[P4-4] mmap LAZY FAILED\n");
		exit(1);
	}

	/* 首次访问：触发页错误 → page_fault_handler → 按需分页 */
	buf[0] = 'P';
	buf[1] = '4';
	buf[2] = '-';
	buf[3] = '4';
	buf[4] = '\n';
	buf[5] = '\0';

	uprint("[P4-4] demand paging OK: wrote '");
	write(1, buf, 5);
	uprint("'\n");

	/* 第二页也触发页错误 */
	buf[4096] = 'o';
	buf[4097] = 'k';
	buf[4098] = '\0';
	uprint("[P4-4] demand paging page 2: ");
	uprint(&buf[4096]);
	uprint("\n");

	munmap((uint64_t)(uintptr_t)buf, 8192);

	/* P5-2a: OOM 故障注入 — 尝试分配远超可用内存的量。
	 * QEMU 分配 512MB，内核+页表已占 ~50MB，512MB mmap 应失败。
	 * 成功 = mmap 返回 NULL（不 crash），失败 = kernel panic。 */
	{
		char *huge = (char *)mmap(512 << 20,
			PROT_READ | PROT_WRITE, MAP_LAZY);
		if (!huge)
			uprint("[P5-2a] OOM: mmap 512MB FAILED (expected)\n");
		else {
			uprint("[P5-2a] OOM: mmap 512MB unexpectedly OK\n");
			munmap((uint64_t)(uintptr_t)huge, 512 << 20);
		}
	}

    uprint("M3c: ELF -> EL0 -> syscall write -> exit, DONE\n");

    exit(0);
}
