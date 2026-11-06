/* 用户程序入口（x86_64 与 ARM64 共用）。
 *
 * 验证 ELF 加载、用户态入口以及 write、sleep、exit 系统调用路径；通过循环
 * 输出和休眠观察系统调用与调度器的交互。另以 MAP_LAZY 验证按需分页：x86_64
 * 由 #PF 处理缺页，ARM64 由 Data Abort 路径处理。
 */

#include <stdint.h>
#include <stddef.h>

#include "user.lib.h"

/* 系统调用封装由 user.lib_arm64.c 或 user.lib.c 提供。 */
int write(int fd, const void *buf, unsigned long count);
void exit(int status) __attribute__((noreturn));
void yield(void);

size_t strlen(const char *s)
{
	size_t n = 0;

	if (!s) {
		return 0;
	}

	while (*s != '\0') {
		n++;
		s++;
	}

	return n;
}

/* 通过 write 输出以 NUL 结尾的字符串。 */
static void uprint(const char *msg)
{
	write(1, msg, strlen(msg) + 1);
}

int main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	uprint("LaOS user ELF: loaded OK\n");
	yield(); /* 验证主动让出 CPU 的系统调用。 */

	uint32_t loops = 0;
	while (loops++ < 2u) {
		uprint("LaOS user ELF: running ...\n");
		msleep(5000);
	}

	/*
	 * 按需分页测试：
	 *
	 * MAP_LAZY 先登记两页 VMA，不立即分配物理页；首次写入触发缺页，
	 * 再由架构缺页处理路径按需分配并映射物理页。
	 */
	char *buf = (char *)mmap(8192, PROT_READ | PROT_WRITE, MAP_LAZY);
	if (!buf) {
		uprint("[P4-4] mmap LAZY FAILED\n");
		exit(1);
	}

	/* 首次访问触发缺页，由架构缺页处理路径分配并映射物理页。 */
	buf[0] = 'P';
	buf[1] = '4';
	buf[2] = '-';
	buf[3] = '4';
	buf[4] = '\n';
	buf[5] = '\0';

	uprint("[P4-4] demand paging OK: wrote '");
	write(1, buf, 5);
	uprint("'\n");

	/* 访问第二页，验证该页也能按需建立映射。 */
	buf[4096] = 'o';
	buf[4097] = 'k';
	buf[4098] = '\0';
	uprint("[P4-4] demand paging page 2: ");
	uprint(&buf[4096]);
	uprint("\n");

	munmap((uint64_t)(uintptr_t)buf, 8192);

	/* 预留较大的惰性映射区间，验证 VMA 分配和释放；此项不测试物理 OOM。 */
	{
		const uint64_t reserve_size = 128ULL << 20;
		char *reserved = (char *)mmap(reserve_size, PROT_READ | PROT_WRITE, MAP_LAZY);
		if (!reserved) {
			uprint("[P5-2a] lazy VMA reservation FAILED\n");
		} else {
			uprint("[P5-2a] lazy VMA reservation OK (no physical allocation)\n");
			munmap((uint64_t)(uintptr_t)reserved, reserve_size);
		}
	}

	uprint("M3c: ELF -> EL0 -> syscall write -> exit, DONE\n");

	exit(0);
}
