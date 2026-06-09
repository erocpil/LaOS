#include "../kernel/printf.h"
#include "../kernel/thread.h"

/* Keep these objects distinct so the module exercises initialized data,
 * NOBITS allocation, local-address materialization, and ABS64 pointers. */
static uint64_t data_word = 0x1122334455667788ULL;
volatile uint64_t module_abi_ldst_word = 9;
volatile uint8_t module_abi_ldst_byte = 0x5a;
static uint64_t bss_words[4];
static const char pass_message[] = "[module-abi] relocation + data + bss, PASS\n";
static uint64_t *volatile data_pointer = &data_word;
static void (*volatile serial_pointer)(char) = serial_putchar;

#if defined(__aarch64__)
extern int32_t module_abi_prel32;
extern void module_abi_tail_exit(int code);

/* Keep PREL32 and JUMP26 in allocated sections so the loader must apply them
 * for execution, rather than merely encountering them in DWARF metadata. */
__asm__(
	".pushsection .rodata.module_abi, \"a\"\n"
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

__attribute__((noinline))
static uint64_t transform(uint64_t value)
{
	return (value ^ 0x1020304050607080ULL) + 7;
}

#if defined(__aarch64__)
__attribute__((noinline))
static uint64_t load_ldst_word(void)
{
	uint64_t value;
	__asm__ volatile(
		"adrp %0, module_abi_ldst_word\n"
		"ldr %0, [%0, #:lo12:module_abi_ldst_word]"
		: "=&r"(value));
	return value;
}

__attribute__((noinline))
static uint8_t load_ldst_byte(void)
{
	uint64_t value;
	__asm__ volatile(
		"adrp %0, module_abi_ldst_byte\n"
		"ldrb %w0, [%0, #:lo12:module_abi_ldst_byte]"
		: "=&r"(value));
	return (uint8_t)value;
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
#endif

static void serial_puts(const char *text)
{
	while (*text)
		serial_pointer(*text++);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	for (unsigned int i = 0; i < 4; i++) {
		if (bss_words[i] != 0) {
			serial_puts("[module-abi] bss initialization, FAIL\n");
			thread_exit(1);
		}
	}

	bss_words[2] = transform(*data_pointer);
	if (bss_words[2] != 0x010203040506070fULL ||
		load_ldst_word() != 9 || load_ldst_byte() != 0x5a) {
		serial_puts("[module-abi] relocation/data check, FAIL\n");
		thread_exit(2);
	}

#if defined(__aarch64__)
	const char *prel_target = (const char *)&module_abi_prel32
		+ module_abi_prel32;
	if (prel_target != pass_message) {
		serial_puts("[module-abi] PREL32 check, FAIL\n");
		thread_exit(3);
	}
#endif

	serial_puts(pass_message);
#if defined(__aarch64__)
	module_abi_tail_exit(0);
#else
	thread_exit(0);
#endif
	return 0;
}
