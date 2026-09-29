/*
 * main.c - LaOS 内核入口点
 *
 * Limine entry，boot 流程编排(PMM->VMM->IDT->GDT->Timer->SMP up->sched).
 * TTY 初始化，module 载入，用户态进程创建等均在 main 中顺序完成。
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

#include "debug.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "gdt.h"
#include "idt.h"
#include "vmm.h"
#include "heap.h"
#include "module_alloc.h"
#include "sched.h"
#include "thread.h"
#include "lapic.h"
#include "cpu.h"
#include "syscall.h"
#include "acpi.h"
#include "pci.h"
#include "task.h"
#include "monitor.h"
#include "tty.h"
#include "hhdm.h"
#include "log.h"
#include "boot_limine.h"
#include "selftest.h"
#include "test_remote_enqueue.h"
#include "test_rcu_publish.h"
#include "test_rcu_stress.h"
#include "test_priority.h"
#include "task_conf.h"
#include "test_registry.h"
#include "virtio_pci.h"

#include "elf.h"

/* Embedded initrd CPIO archive (user ELF + configs) */
#include "initrd_embed.h"
#include "cpio.h"
#include "test_vma.h"
#include "test_cpio.h"
#include "test_lafs.h"
#include "test_block_device.h"
#include "lafs.h"
#include "block_device.h"

extern const char git_version[];
#include "config.h"

/*
 * exception_test -- 触发异常以验证异常处理器输出完整性
 *
 * 三层 noinline 调用链保证栈回溯至少显示 3 帧，每帧都能被 kallsyms_lookup 解析。
 * 由 CONFIG_EXCEPTION_TEST 开关控制，默认关闭。
 * 用 CONFIG_PF_TEST 选择异常类型： 0=#DE(除零)， 1=#PF(空指针)
 *
 * 验证项(#DE): GPR 全量 dump， CR0/CR4/RFLAGS 位解码， RIP 指令字节， 符号解析， 栈回溯
 * 验证项(#PF): CR0/CR4 位解码， #PF error code 解码， 四级页表遍历， 符号解析， 栈回溯
 */
#if CONFIG_EXCEPTION_TEST

/* 最内层：根据 CONFIG_PF_TEST 触发 #DE 或 #PF */
__attribute__((noinline)) void exception_test_trigger(void)
{
#if CONFIG_PF_TEST
	kprintf("  Triggering: page fault at 0x1000\n");
	volatile int *p = (volatile int *)0x1000;
	*p = 42; /* #PF: Page Fault */
#else
	volatile int a = 1;
	volatile int b = 0;

	kprintf("  Triggering: %d / %d = ...\n", a, b);
	volatile int c = a / b; /* #DE: Divide Error */
	(void)c;
#endif
}
EXPORT_SYMBOL(exception_test_trigger);

/* 中间层 */
__attribute__((noinline)) void exception_test_lv2(void)
{
	exception_test_trigger();
}
EXPORT_SYMBOL(exception_test_lv2);

/* 最外层(从 kmain 调用) */
__attribute__((noinline)) void exception_test_lv1(void)
{
	exception_test_lv2();
}
EXPORT_SYMBOL(exception_test_lv1);

static void exception_test(void)
{
	kprintf("\n");
	kprintf("========================================\n");
#if CONFIG_PF_TEST
	kprintf("  EXCEPTION HANDLER TEST: #PF (NULL deref)\n");
	kprintf("========================================\n");
	kprintf("  Expect: CR0/CR4 decode, page table walk,\n");
	kprintf("  #PF error code, symbol resolution,\n");
	kprintf("  stack backtrace with symbols.\n");
#else
	kprintf("  EXCEPTION HANDLER TEST: #DE (div0)\n");
	kprintf("========================================\n");
	kprintf("  Expect: GPR dump, CR0/CR4/RFLAGS decode,\n");
	kprintf("  RIP code bytes, symbol resolution,\n");
	kprintf("  stack backtrace with symbols.\n");
#endif
	kprintf("========================================\n");

	exception_test_lv1();

	/* 不会执行到这里 */
	kprintf("  UNEXPECTED: division did not trap!\n");
}
#endif

// 设 base revision 为 6:当前 Limine 引导协议推荐的值。

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

// Limine 请求可放在任何位置，但必须防止编译器优化掉(volatile 或 used 属性).

// 声明模块请求
__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
	.id = LIMINE_MODULE_REQUEST_ID,
	.revision = 0
};

// 定义 Limine 请求(必须设为 volatile 且由 static 修饰，防止编译器优化)
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
	.id = LIMINE_FRAMEBUFFER_REQUEST_ID,
	.revision = 0
};

// Limine 请求的起始/结束标记，可放在任意 .c 文件中。

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

// GCC / Clang 可能优化掉未显式引用的 Limine 请求变量，用 used 属性强制保留。
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
	.id = LIMINE_MEMMAP_REQUEST_ID,
	.revision = 0
};

// HHDM 请求：请求引导程序将全部物理内存映射到虚拟地址空间的高半区
__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
	.id = LIMINE_HHDM_REQUEST_ID,
	.revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_mp_request smp_request = {
	.id = LIMINE_MP_REQUEST_ID,
	.revision = 0
};

// 定义 Limine RSDP 请求
// 使用 volatile 和 static 确保它存在于内核数据段中，且不被优化
__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
	.id = LIMINE_RSDP_REQUEST_ID,
	.revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_dtb_request dtb_request = {
	.id = LIMINE_DTB_REQUEST_ID,
	.revision = 0
};

static struct boot_info boot_info;

/* Limine's lapic_id is a hardware identifier and may be sparse.  Keep it
 * separate from the dense logical CPU index used by per-CPU arrays. */
static uint32_t logical_cpu_index(uint32_t lapic_id)
{
	struct limine_mp_response *r = smp_request.response;
	if (!r) {
		return 0;
	}
	for (uint64_t i = 0; i < r->cpu_count && i < MAX_CPUS; i++) {
		if (r->cpus[i]->lapic_id == lapic_id) return (uint32_t)i;
	}

	return 0;
}

// 每个从核被唤醒后执行的第一段 C 代码
void secondary_cpu_init(struct limine_mp_info *info)
{
	// 禁用中断直到初始化完成
	__asm__ volatile ("cli");

	uint32_t id = logical_cpu_index(info->lapic_id);

	cpu_early_init_gs(id);

	vmm_init(id);

	gdt_init_dynamic(id);

	idt_ap_init();

	lapic_init();

	per_cpu_init(id, true);

	syscall_init();

	wait_online(id);

	task_run();

	__asm__ volatile ("sti");

	schedule();

	panic();
}

void start_smp(void)
{
	L_TAG(LOG_CPU, "Bringing up application processors.\n");
	struct limine_mp_response *smp_response = smp_request.response;
	if (smp_response == NULL) {
		L_TAG(LOG_CPU, "SMP not supported.\n");
		return;
	}

	L_TAG(LOG_BOOT, "Starting smp on %d CPUs.\n", g_cpu_count);
	for (uint64_t i = 0; i < g_cpu_count; i++) {
		struct limine_mp_info *cpu = smp_response->cpus[i];

		// 为每个核准备环境，需要手动通过 goto_address 启动
		if (cpu->lapic_id != smp_response->bsp_lapic_id) {
			// 设置 goto_address 后，Limine 会让该核跳转到那里
			cpu->goto_address = secondary_cpu_init;
		} else {
			gdt_init_dynamic(g_cpu_count);
			per_cpu_init(logical_cpu_index(smp_response->bsp_lapic_id), false);
		}
	}
}

// 声明 Limine stack size request
__attribute__((used, section(".limine_requests")))
static volatile struct limine_stack_size_request stack_request = {
	.id = LIMINE_STACK_SIZE_REQUEST_ID,
	.revision = 0,
	.stack_size = 65536  // 64KB，按需调整
};

uintptr_t get_mcfg_from_limine(void)
{
	// 检查 Limine 是否成功填充了响应
	if (rsdp_request.response == NULL || rsdp_request.response->address == NULL) {
		// 如果失败，可能说明该机器不支持 ACPI (极罕见)
		return 0;
	}

	// 获取 RSDP 的地址
	// Limine 传回的是经过虚拟地址偏移后的指针(通常在 HHDM 区域)
	rsdp_t *rsdp = (rsdp_t*)rsdp_request.response->address;

	// 调用之前写的 find_mcfg_base
	uintptr_t base_address = find_mcfg_base(rsdp);
	if (!base_address) {
		panic("ERROR: get_mcfg_from_limine()");
	}

	return base_address;
}

void check_bootloader(void)
{
	L("Checking limine ...\n");
	// 确认 bootloader 支持请求的 base revision.
	if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
		panic("ERROR: BASE REVISION SUPPORTED!");
	}

	// 确认 framebuffer 可用。
	if (framebuffer_request.response == NULL
			|| framebuffer_request.response->framebuffer_count < 1) {
		panic("ERROR: Framebuffer is NULL or 0!");
	}
	// 取第一个 framebuffer.
	fb_set_info(framebuffer_request.response->framebuffers[0]);
	struct limine_framebuffer *framebuffer = (struct limine_framebuffer*)fb_get_info();
	uint32_t screen_width = framebuffer->width;
	uint32_t screen_height = framebuffer->height;
	L("frame buffer count %ld screen width %u height %u.\n",
			framebuffer_request.response->framebuffer_count,
			screen_width, screen_height);

	if (hhdm_request.response == NULL) {
		panic("ERROR: HHDM response is NULL!");
	}
	hhdm_init(hhdm_request.response->offset);

	if (memmap_request.response == NULL) {
		panic("ERROR: Memory map response is NULL!");
	}
	if (boot_info_from_limine(&boot_info, hhdm_request.response,
				memmap_request.response, module_request.response,
				dtb_request.response) != 0) {
		panic("ERROR: invalid Limine boot information");
	}

	struct limine_mp_response *smp_response = smp_request.response;
	/* P0-2: APIC IDs may be sparse (0,2,4...) — cap cpu_count at
	 * MAX_CPUS to prevent array overflow. Also handle NULL response
	 * (no SMP hw) by falling back to single-CPU. */
	if (smp_response == NULL) {
		L_TAG(LOG_BOOT, "WARNING: SMP not supported.\n");
		g_cpu_count = 1;
	} else {
		g_cpu_count = smp_response->cpu_count;
		if (g_cpu_count > MAX_CPUS) {
			L_TAG(LOG_BOOT, "WARNING: %lu CPUs reported, capping at %u.\n",
					smp_response->cpu_count, MAX_CPUS);
			g_cpu_count = MAX_CPUS;
		}
		L("Found %d CPUs.\n", g_cpu_count);
	}

	L_TAG(LOG_BOOT, "--- LaOS Kernel Booting ---\n");
	L_TAG(LOG_BOOT, "[git] %s\n", git_version);
}

/* ELF 用户程序加载 (x86_64) */

#define X86_EMBEDDED_USER_STACK_TOP  0x70000000ULL
#define USER_STACK_PAGES 16

/*
 * load_user_elf() — 解析 x86_64 ELF，映射 LOAD 段到用户 VA，
 * 拷贝段数据、清零 BSS，返回 ELF 入口地址。
 */
static uint64_t load_user_elf(uint8_t *elf_raw, uint32_t elf_size)
{
	(void)elf_size;
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)elf_raw;

	if (elf_check(ehdr) != 0) {
		kprintf("[ELF] header check failed\n");
		return 0;
	}
	kprintf("[ELF] machine=%u entry=0x%lx phnum=%u\n",
			ehdr->e_machine, ehdr->e_entry, ehdr->e_phnum);

	Elf64_Phdr *phdrs = (Elf64_Phdr*)((uint8_t*)ehdr + ehdr->e_phoff);

	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type != PT_LOAD) continue;

		uint64_t vaddr = phdrs[i].p_vaddr;
		uint64_t filesz = phdrs[i].p_filesz;
		uint64_t memsz = phdrs[i].p_memsz;
		uint32_t flags = phdrs[i].p_flags;

		kprintf("[ELF] LOAD va=0x%lx fs=0x%lx ms=0x%lx fl=%x\n",
				vaddr, filesz, memsz, flags);

		uint64_t vbase = vaddr & ~0xFFFULL;
		uint64_t npages = (vaddr + memsz - vbase + 0xFFF) >> 12;

		for (uint64_t p = 0; p < npages; p++) {
			uint64_t page_va = vbase + p * 0x1000;
			uint64_t phys = (uint64_t)pmm_alloc();
			if (!phys) panic("pmm_alloc ELF page");

			uint64_t pte_flags = PTE_PRESENT | PTE_WRITABLE;
			if (!(flags & PF_W)) {
				pte_flags &= ~PTE_WRITABLE;
			}
			if (!(flags & PF_X)) {
				pte_flags |= PTE_NX;
			}

			vmm_map_user((uint64_t*)virt_to_phys(kernel_pml4),
					page_va, phys, pte_flags);

			uint8_t *dest = (uint8_t*)phys_to_virt(phys);
			uint64_t page_off = (p == 0) ? (vaddr & 0xFFF) : 0;

			uint64_t seg_pos = p * 0x1000;
			uint64_t seg_end = memsz;
			uint64_t page_end = seg_pos + 0x1000;
			if (page_end > seg_end) page_end = seg_end;

			uint64_t copy_n = 0;
			if (seg_pos < filesz) {
				copy_n = filesz - seg_pos;
				if (copy_n > page_end - seg_pos) {
					copy_n = page_end - seg_pos;
				}
				uint64_t src_off = phdrs[i].p_offset + seg_pos;
				memcpy(dest + page_off, (uint8_t*)ehdr + src_off, copy_n);
			}
			uint64_t fill_n = page_end - seg_pos;
			if (copy_n < fill_n) {
				memset(dest + page_off + copy_n, 0, fill_n - copy_n);
			}
		}
	}

	/* 用户栈：映射 USER_STACK_PAGES 页 */
	{
		uint64_t stack_base = X86_EMBEDDED_USER_STACK_TOP - USER_STACK_PAGES * 0x1000;
		for (int i = 0; i < USER_STACK_PAGES; i++) {
			uint64_t page_va = stack_base + i * 0x1000;
			uint64_t phys = (uint64_t)pmm_alloc();
			if (!phys) {
				panic("pmm_alloc stack");
			}
			vmm_map_user((uint64_t *)virt_to_phys(kernel_pml4),
					page_va, phys, PTE_PRESENT | PTE_WRITABLE);
			memset((void *)phys_to_virt(phys), 0, 0x1000);
		}
	}

	kprintf("[ELF] loaded, entry=0x%lx\n", ehdr->e_entry);

	return ehdr->e_entry;
}

#include "entry_arch.h"

/* ELF 用户线程：进入 Ring 3 执行用户程序 */
static void elf_user_entry(void *arg)
{
	uint64_t entry = (uint64_t)arg;
	kprintf("[usr] entering ELF @ 0x%lx...\n", entry);

	/*
	 * 在栈上布置 iretq 帧：[RIP][CS][RFLAGS][RSP][SS]
	 * 同时在 RSP 下方放置 argc=0, argv=NULL 供 crt0 使用。
	 */
	uint64_t user_sp = X86_EMBEDDED_USER_STACK_TOP;

	/* crt0 从栈上 pop argc，所以先 push argc=0 和 NULL terminator */
	uint64_t *usp = (uint64_t*)(uint64_t)(user_sp - 16);
	usp[0] = 0; /* NULL — argv terminator */
	usp[1] = 0; /* argc = 0 */

	/* iretq 帧 */
	uint64_t *frame = usp - 5;
	frame[0] = entry; /* RIP */
	frame[1] = 0x23; /* CS  = USER_CS */
	frame[2] = 0x202; /* RFLAGS (IF=1) */
	frame[3] = (uint64_t)usp; /* RSP (指向 argc 上方，crt0 第一句 pop rdi) */
	frame[4] = 0x1B; /* SS  = USER_DS */

	arch_enter_usermode(frame);
}

// 在 .bss 或全局区声明自己的内核栈
static uint8_t kernel_stack[16384] __attribute__((aligned(16)));

void kmain(void)
{
	log_init();

	check_bootloader();

	tty_init(); /* 尽早初始化以捕获启动日志到 TTY 0 网格 */

	/*
	 * 最早开启 FPU 支持：在 BSP 的任何上下文切换发生之前设置
	 * CR4.OSFXSR。AP 在各自的 lapic_init() 中也会调用。
	 */
	extern void cpu_enable_fpu(void);
	cpu_enable_fpu();

	cpu_early_init_gs(0);

	gdt_init_cpu(0, (uint64_t)(kernel_stack + sizeof(kernel_stack)));

	idt_init();

	pic_disable();

	pmm_init_from_memmap(&boot_info.memory_map);

	vmm_init(0);

	kheap_init();

	module_alloc_init();

	lapic_map(kernel_pml4);

	pcie_init(get_mcfg_from_limine());

	lapic_init();

	ioapic_init();

	syscall_init();

	/* VMA/CPIO 单元测试（在线程/调度器启动前） */
	test_lafs_init();
	test_block_device_init();

	kprintf("[unit] VMA test...\n");
	if (!test_vma_run()) {
		kprintf("[unit] WARNING: VMA test FAILED\n");
	}
	kprintf("[unit] CPIO test...\n");
	if (!test_cpio_run()) {
		kprintf("[unit] WARNING: CPIO test FAILED\n");
	}
	kprintf("[unit] LaFS test...\n");
	if (!test_lafs_run()) {
		kprintf("[unit] WARNING: LaFS test FAILED\n");
	}
	kprintf("[unit] block_device test...\n");
	if (!test_block_device_run()) {
		kprintf("[unit] WARNING: block_device test FAILED\n");
	}

	/* Tear down unit-test stubs so real drivers have a clean registry. */
	block_device_reset();

	pci_init();

	virtio_pci_blk_init();

	/* Verify real virtio-blk → LaFS mount + read /etc/motd. */
	if (block_device_count() > 0) {
		if (lafs_mount() == 0) {
			kprintf("[virtio] real virtio-blk LaFS mounted\n");
			char buf[128];
			int ino = lafs_open("/etc/motd");
			if (ino >= 0) {
				int n = lafs_read(ino, buf, 0, sizeof(buf) - 1);
				if (n > 0) {
					buf[n] = '\0';
					kprintf("[virtio] /etc/motd: %s", buf);
				}
			}
		} else {
			kprintf("[virtio] WARNING: real virtio-blk LaFS mount failed\n");
		}
	} else {
		kprintf("[virtio] no block device, skipping LaFS mount\n");
	}

	/* Cache e1000 IRQ line for direct dispatch in irq_handler. */
	{
		struct pci_device *dev;
		list_for_each_entry(dev, &g_pci_devices, node) {
			if (dev->vendor_id == 0x8086 &&
					dev->device_id == 0x100E) {
				idt_register_e1000_irq(dev->irq_line);
				break;
			}
		}
	}

	start_smp();

	task_init(&boot_info.modules);

	thread_init_main();

	kheap_test();

	wait_online(0);

	/* Run registry selfcheck before module loading. */
	test_registry_init();
	test_remote_enqueue_init();
	test_rcu_publish_init();
	test_rcu_stress_init();
	test_priority_init();

	/* Load test payload .mo modules from directive records,
	 * then apply configuration.  Payload init happens synchronously
	 * (selftest_init → selftest_register). */
	if (task_conf_has_directives()) {
		selftest_load_payloads(&boot_info.modules,
				task_conf_get_directives());
		selftest_apply_all(task_conf_get_directives());
	}
	selftest_run();

	/* M3c: 从 initrd CPIO 提取并加载用户 ELF */
	if (cpio_init(initrd_cpio_bin, initrd_cpio_bin_len) != 0) {
		panic("cpio_init initrd");
	}

	struct cpio_entry uelf;
	if (cpio_find("user.elf", &uelf) != 1) {
		panic("user.elf not found in initrd");
	}

	kprintf("[ELF] loading user.elf from initrd (%u bytes)...\n", uelf.size);
	uint64_t user_entry = load_user_elf((uint8_t *)uelf.data, uelf.size);
	if (!user_entry) {
		panic("load_user_elf failed");
	}

	{
		struct thread *ut = thread_create(elf_user_entry, (void*)user_entry);
		if (ut) {
			ut->is_user = 1;
			ut->pml4_phys = (uint64_t*)virt_to_phys(kernel_pml4);
			kprintf("[ELF] user thread created (tid=%lu)\n", ut->id);
		} else {
			kprintf("[ELF] WARNING: thread_create failed\n");
		}
	}

	task_run();

	__asm__ volatile ("sti");

#if CONFIG_EXCEPTION_TEST
	exception_test();
	/* 不会执行到这里，exception_test() 触发 #DE 后系统 halt */
	kprintf("BUG: exception_test returned!\n");
	hcf();
#endif

	L_TAG(LOG_BOOT, "LaOS is running ...\n");

	start_monitor();

	uint64_t clear = 0;
	while (1) {
		if (0 == (clear++ % 100)) {
			if (atomic_read(&current_tty_id) == get_current()->tty_id) {
#if CONFIG_DEBUG
				fb_clear_screen(0);
#endif
			}
		}
		schedule_timeout(1);
	}

	hcf();
}
