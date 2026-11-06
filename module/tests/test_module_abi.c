#include "printf.h"
#include "thread.h"

/* 保持这些对象相互独立，以覆盖已初始化数据、NOBITS 分配、本地地址解析及 ABS64 指针。 */
static uint64_t data_word = 0x1122334455667788ULL;
volatile uint64_t module_abi_ldst_word = 9;
volatile uint32_t module_abi_ldst_dword = 0x12345678U;
volatile uint8_t module_abi_ldst_byte = 0x5a;
static uint64_t bss_words[4];
static const char pass_message[] = "[module-abi] relocation + data + bss, PASS\n";
static uint64_t *volatile data_pointer = &data_word;
/* 保留指向内核导入函数的 ABS64 指针，供运行时验证重定位结果。 */
static void (*volatile print_pointer)(const char *, ...) = kprintf;

#if defined(__aarch64__)
extern int32_t module_abi_prel32;
extern void module_abi_tail_exit(int code);

/* 将 PREL32 和 JUMP26 重定位保留在可分配节中，确保载入器执行时必须处理它们，
 * 而非仅在 DWARF 调试信息中遇到这些类型。 */
__asm__(".pushsection .rodata.module_abi, \"a\"\n"
	".balign 4\n"
	".global module_abi_prel32\n"
	"module_abi_prel32:\n"
	".word pass_message - .\n"
	".popsection\n"
	".pushsection .text.module_abi, \"ax\"\n"
	".balign 4\n"
	".global module_abi_tail_exit\n"
	".type module_abi_tail_exit, %function\n"
	"module_abi_tail_exit:\n"
	"b thread_exit\n"
	".size module_abi_tail_exit, . - module_abi_tail_exit\n"
	".popsection\n");
#endif

__attribute__((noinline)) static uint64_t transform(uint64_t value)
{
	return (value ^ 0x1020304050607080ULL) + 7;
}

#if defined(__aarch64__)
__attribute__((noinline)) static uint64_t load_ldst_word(void)
{
	uint64_t value;
	__asm__ volatile("adrp %0, module_abi_ldst_word\n"
			 "ldr %0, [%0, #:lo12:module_abi_ldst_word]"
		: "=&r"(value));
	return value;
}

__attribute__((noinline)) static uint8_t load_ldst_byte(void)
{
	uint64_t value;
	__asm__ volatile("adrp %0, module_abi_ldst_byte\n"
			 "ldrb %w0, [%0, #:lo12:module_abi_ldst_byte]"
		: "=&r"(value));
	return (uint8_t)value;
}

__attribute__((noinline)) static uint32_t load_ldst_dword(void)
{
	uint64_t value;
	__asm__ volatile("adrp %0, module_abi_ldst_dword\n"
			 "ldr %w0, [%0, #:lo12:module_abi_ldst_dword]"
		: "=&r"(value));
	return (uint32_t)value;
}
#else
static uint64_t load_ldst_word(void)
{
	return module_abi_ldst_word;
}

static uint8_t load_ldst_byte(void)
{
	return module_abi_ldst_byte;
}

static uint32_t load_ldst_dword(void)
{
	return module_abi_ldst_dword;
}
#endif

static void print_message(const char *text)
{
	print_pointer("%s", text);
}

int laos_module_entry(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	for (unsigned int i = 0; i < 4; i++) {
		if (bss_words[i] != 0) {
			print_message("[module-abi] bss initialization, FAIL\n");
			thread_exit(1);
		}
	}

	bss_words[2] = transform(*data_pointer);
	if (bss_words[2] != 0x010203040506070fULL || load_ldst_word() != 9 ||
		load_ldst_dword() != 0x12345678U || load_ldst_byte() != 0x5a) {
		print_message("[module-abi] relocation/data check, FAIL\n");
		thread_exit(2);
	}

#if defined(__aarch64__)
	const char *prel_target = (const char *)&module_abi_prel32 + module_abi_prel32;
	if (prel_target != pass_message) {
		print_message("[module-abi] PREL32 check, FAIL\n");
		thread_exit(3);
	}
#endif

	print_message(pass_message);
#if defined(__aarch64__)
	module_abi_tail_exit(0);
#else
	thread_exit(0);
#endif
	return 0;
}
