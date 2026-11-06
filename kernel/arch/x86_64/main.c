/*
 * x86_64 内核入口与启动阶段编排。
 *
 * 初始化处理器与内存管理基础设施，启动 AP，建立调度环境，并按启动用途
 * 配置设备、模块和测试。具体子系统工作由对应初始化函数负责。
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
#include "arch_cpu.h"
#include "syscall.h"
#include "acpi.h"
#include "pci.h"
#include "task.h"
#include "monitor.h"
#include "tty.h"
#include "hhdm.h"
#include "log.h"
#include "boot_limine.h"
#include "boot_phase.h"
#include "tests/runner.h"
#include "tests/test_exception.h"
#include "task_conf.h"
#include "virtio_pci.h"

#include "boot_files.h"
#include "lafs.h"
#include "block_device.h"

extern const char git_version[];
#include "config.h"

/* 请求 Limine base revision 6，以使用本内核依赖的协议接口。 */

__attribute__((used, section(".limine_requests"))) static volatile uint64_t limine_base_revision[] =
	LIMINE_BASE_REVISION(6);

/* Limine 请求对象必须保留在请求段中，不能被编译器移除。 */

/* 请求引导器提供模块列表。 */
__attribute__((used, section(".limine_requests"))) static volatile struct limine_module_request
	module_request = {.id = LIMINE_MODULE_REQUEST_ID, .revision = 0};

/* 请求 framebuffer 信息。 */
__attribute__((used, section(".limine_requests"))) static volatile struct limine_framebuffer_request
	framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

/* Limine 请求段的边界标记。 */

__attribute__((used, section(".limine_requests_start"))) static volatile uint64_t
	limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used,
	section(".limine_requests_end"))) static volatile uint64_t limine_requests_end_marker[] =
	LIMINE_REQUESTS_END_MARKER;

// GCC / Clang 可能优化掉未显式引用的 Limine 请求变量，用 used 属性强制保留。
__attribute__((used, section(".limine_requests"))) static volatile struct limine_memmap_request
	memmap_request = {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0};

// HHDM 请求：请求引导程序将全部物理内存映射到虚拟地址空间的高半区
__attribute__((used,
	section(".limine_requests"))) static volatile struct limine_hhdm_request hhdm_request = {
	.id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

__attribute__((
	used, section(".limine_requests"))) static volatile struct limine_mp_request smp_request = {
	.id = LIMINE_MP_REQUEST_ID, .revision = 0};

// 定义 Limine RSDP 请求
// 使用 volatile 和 static 确保它存在于内核数据段中，且不被优化
__attribute__((used,
	section(".limine_requests"))) static volatile struct limine_rsdp_request rsdp_request = {
	.id = LIMINE_RSDP_REQUEST_ID, .revision = 0};

__attribute__((used,
	section(".limine_requests"))) static volatile struct limine_dtb_request dtb_request = {
	.id = LIMINE_DTB_REQUEST_ID, .revision = 0};

__attribute__((
	used, section(".limine_requests"))) static volatile struct limine_executable_cmdline_request
	cmdline_request = {.id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0};

static struct boot_info boot_info;
static struct task_conf test_conf;
static void launch_boot_tasks(void);

/* Limine 的 lapic_id 是可能不连续的硬件标识；per-CPU 数组使用连续的逻辑索引，
 * 两者分别保存和使用。 */
static uint32_t logical_cpu_index(uint32_t lapic_id)
{
	struct limine_mp_response *r = smp_request.response;
	if (!r) {
		return 0;
	}
	for (uint64_t i = 0; i < r->cpu_count && i < MAX_CPUS; i++) {
		if (r->cpus[i]->lapic_id == lapic_id) {
			return (uint32_t)i;
		}
	}

	return 0;
}

// 每个从核被唤醒后执行的第一段 C 代码
void secondary_cpu_init(struct limine_mp_info *info)
{
	// 禁用中断直到初始化完成
	__asm__ volatile("cli");

	uint32_t id = logical_cpu_index(info->lapic_id);

	cpu_early_init_gs(id);

	vmm_init(id);

	gdt_init_dynamic(id);

	idt_ap_init();

	lapic_init();

	per_cpu_init(id, CPU_STARTUP_AP);

	syscall_init();

	wait_online(id);
	boot_phase_wait_for(BOOT_PHASE_SERVICES_READY);

	launch_boot_tasks();

	cpu_assert_sched_context_ready();
	__asm__ volatile("sti");

	schedule();

	panic();
}

void start_smp(void)
{
	L_TAG(LOG_CPU, "Bringing up application processors.\n");
	struct limine_mp_response *smp_response = smp_request.response;
	uint32_t bsp_id = smp_response ? logical_cpu_index(smp_response->bsp_lapic_id) : 0;

	/* BSP 先离开早期上下文，并完成共享 GDT 分配，再允许 AP 进入内核。 */
	gdt_init_dynamic(g_cpu_count);
	per_cpu_init(bsp_id, CPU_STARTUP_BSP);
	thread_init_main();
	cpu_assert_sched_context_ready();
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
		}
	}
}

// 声明 Limine stack size request
__attribute__((used, section(".limine_requests"))) static volatile struct limine_stack_size_request
	stack_request = {
		.id = LIMINE_STACK_SIZE_REQUEST_ID,
		.revision = 0,
		.stack_size = 65536 // 64KB，按需调整
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
	rsdp_t *rsdp = (rsdp_t *)rsdp_request.response->address;

	// 调用之前写的 find_mcfg_base
	uintptr_t base_address = find_mcfg_base(rsdp, &boot_info.memory_map);
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
	if (framebuffer_request.response == NULL ||
		framebuffer_request.response->framebuffer_count < 1) {
		panic("ERROR: Framebuffer is NULL or 0!");
	}
	// 取第一个 framebuffer.
	fb_set_info(framebuffer_request.response->framebuffers[0]);
	struct limine_framebuffer *framebuffer = (struct limine_framebuffer *)fb_get_info();
	uint32_t screen_width = framebuffer->width;
	uint32_t screen_height = framebuffer->height;
	L("frame buffer count %ld screen width %u height %u.\n",
		framebuffer_request.response->framebuffer_count, screen_width, screen_height);

	if (hhdm_request.response == NULL) {
		panic("ERROR: HHDM response is NULL!");
	}
	hhdm_init(hhdm_request.response->offset);

	if (memmap_request.response == NULL) {
		panic("ERROR: Memory map response is NULL!");
	}
	if (boot_info_from_limine(&boot_info, hhdm_request.response, memmap_request.response,
		    module_request.response, dtb_request.response) != 0) {
		panic("ERROR: invalid Limine boot information");
	}

	struct limine_mp_response *smp_response = smp_request.response;
	/* APIC ID 不用作数组索引。限制逻辑 CPU 数量不超过 MAX_CPUS；无 SMP 响应时
	 * 按单 CPU 启动。 */
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

/* 挂载 LaFS 并读取 /etc/motd，以验证 virtio-blk 设备的实际访问路径。 */
static void verify_virtio_lafs(void)
{
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
}

/* 注册 e1000 中断向量，供 irq_handler() 分发。 */
static void register_e1000_irq(void)
{
	struct pci_device *dev;
	list_for_each_entry(dev, &g_pci_devices, node) {
		if (dev->vendor_id == 0x8086 && dev->device_id == 0x100E) {
			idt_register_e1000_irq(dev->irq_line);
			break;
		}
	}
}

/* Base 与 compatibility 是显式启动用途，配置来源不会相互回退。 */
static bool compatibility_boot;
static bool testing_boot;
static struct boot_file_set test_files;
static struct boot_module test_modules[64];
static struct boot_module_list test_module_list;
static struct boot_file_set base_files;

static void initialize_boot_tasks(void)
{
	const char *cmdline = cmdline_request.response ? cmdline_request.response->cmdline : NULL;
	if (cmdline && !strcmp(cmdline, "boot=compat")) {
		compatibility_boot = true;
		if (task_init(&boot_info.modules)) {
			panic("compat boot configuration failed");
		}
		return;
	}
	testing_boot = cmdline && !strcmp(cmdline, "boot=test");
	if (!testing_boot && cmdline && *cmdline && strcmp(cmdline, "boot=base")) {
		panic("unsupported boot mode");
	}
	struct boot_file_set transport;
	struct boot_file archive;
	if (boot_files_from_modules(&transport, "limine-transport", &boot_info.modules) ||
		boot_file_lookup(&transport, "/boot/initrd.cpio", &archive)) {
		panic("base initrd missing or ambiguous");
	}
	if (boot_files_from_cpio(&base_files, "base-initrd", archive.data, archive.size) ||
		task_init_ctx(&task_conf, &base_files, TASK_CONF_BASE, "/conf/task.conf")) {
		panic("base initrd configuration failed");
	}
	struct task *task;
	list_for_each_entry(task, &task_conf.head, node) {
		struct boot_file file;
		if (task->cpu_id < 0 || (uint32_t)task->cpu_id >= g_cpu_count ||
			task->type < TASK_DRIVER || task->type > TASK_USER ||
			boot_file_lookup_in(&base_files, "/task", task->module, &file)) {
			panic("invalid base task or missing component: %s", task->module);
		}
	}
	kprintf("[boot-config] base initrd ready (%lu bytes)\n", (uint64_t)archive.size);
	if (testing_boot) {
		for (uint64_t i = 0; i < boot_info.modules.count; i++) {
			char path[BOOT_FILE_PATH_MAX];
			if (boot_file_normalize(boot_info.modules.items[i].path, path)) {
				panic("invalid test transport path");
			}
			if (!strcmp(path, "/boot/initrd.cpio")) {
				continue;
			}
			if (test_module_list.count == 64) {
				panic("too many test modules");
			}
			test_modules[test_module_list.count++] = boot_info.modules.items[i];
		}
		test_module_list.items = test_modules;
		if (boot_files_from_modules(&test_files, "limine-tests", &test_module_list) ||
			task_init_ctx(&test_conf, &test_files, TASK_CONF_TEST, "/conf/test.conf")) {
			panic("test boot configuration failed");
		}
	}
}

static void launch_boot_tasks(void)
{
	task_run();
	if (testing_boot && task_run_configured_cpu_ctx(&test_conf, cpu_get_ctx()->id)) {
		panic("test tasks already submitted or invalid");
	}
}

// 在 .bss 或全局区声明自己的内核栈
static uint8_t kernel_stack[16384] __attribute__((aligned(16)));

void kmain(void)
{
	/* BSP 的启动代码在本地 IRQ 关闭状态下运行；服务和调度上下文就绪后再开放。 */
	__asm__ volatile("cli" ::: "memory");
	cpu_early_init_gs(0);

	log_init();

	check_bootloader();

	tty_init(); /* 尽早初始化以捕获启动日志到 TTY 0 网格 */

	/*
	 * 最早开启 FPU 支持：在 BSP 的任何上下文切换发生之前设置
	 * CR4.OSFXSR。AP 在各自的 lapic_init() 中也会调用。
	 */
	cpu_enable_fpu();

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
	boot_phase_complete(BOOT_PHASE_MEMORY_READY);

	/* 启动阶段自测由测试子系统统一执行。 */
	kernel_tests_run_bootstrap();

	pci_init();

	virtio_pci_blk_init();

	verify_virtio_lafs();

	register_e1000_irq();
	boot_phase_complete(BOOT_PHASE_DEVICES_READY);

	start_smp();

	initialize_boot_tasks();

	kheap_test();
	boot_phase_complete(BOOT_PHASE_RUNTIME_READY);

	wait_online(0);
	boot_phase_complete(BOOT_PHASE_SMP_READY);

	/* 所有 CPU 上线后注册并启动配置中的自测。 */
	if (testing_boot) {
		if (kernel_tests_start_config(&test_conf)) {
			panic("test session startup failed");
		}
	} else if (compatibility_boot) {
		kernel_tests_start(&boot_info.modules);
	}

	launch_boot_tasks();
	boot_phase_complete(BOOT_PHASE_SERVICES_READY);

	cpu_assert_sched_context_ready();
	__asm__ volatile("sti");

	test_exception_run();

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
