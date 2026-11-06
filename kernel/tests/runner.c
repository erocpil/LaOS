#include "runner.h"

#include "selftest.h"
#include "task_conf.h"
#include "printf.h"
#include "block_device.h"
#include "boot_phase.h"
#include "cpio.h"
#include "lafs.h"
#include "debug.h"
#include "test_block_device.h"
#include "test_cpio.h"
#include "test_lafs.h"
#include "test_registry.h"
#include "test_vma.h"
#if defined(__x86_64__)
#include "test_user_loader.h"
#endif

void kernel_tests_run_bootstrap(void)
{
	boot_phase_require(BOOT_PHASE_MEMORY_READY);
	if (interrupts_enabled() || block_device_count() != 0) {
		panic("bootstrap tests require IRQs disabled and an empty device registry");
	}

	test_vma_init();
	test_cpio_init();
	test_lafs_init();
	test_block_device_init();
	bool ok = true;

	kprintf("[unit] VMA test...\n");
	if (!test_vma_run()) {
		kprintf("[unit] WARNING: VMA test FAILED\n");
		ok = false;
	}
	kprintf("[unit] CPIO test...\n");
	if (!test_cpio_run()) {
		kprintf("[unit] WARNING: CPIO test FAILED\n");
		ok = false;
	}
	kprintf("[unit] LaFS test...\n");
	if (!test_lafs_run()) {
		kprintf("[unit] WARNING: LaFS test FAILED\n");
		ok = false;
	}
	kprintf("[unit] block_device test...\n");
	if (!test_block_device_run()) {
		kprintf("[unit] WARNING: block_device test FAILED\n");
		ok = false;
	}

#if defined(__x86_64__)
	if (!test_user_loader_run()) {
		kprintf("[unit] WARNING: user loader test FAILED\n");
		ok = false;
	}
#endif

	/* 运行时 @test 条目复用此处的结果，避免重复执行测试夹具。 */
	cpio_reset();
	lafs_reset();
	block_device_reset();
	struct cpio_entry entry;
	if (block_device_count() != 0 || lafs_open("/") != -1 ||
		cpio_find("user.elf", &entry) != -1) {
		panic("bootstrap test state was not cleared");
	}
	if (!ok) {
		panic("bootstrap tests failed");
	}
	boot_phase_complete(BOOT_PHASE_BOOTSTRAP_TESTS_DONE);
}

static void register_builtin_tests(void)
{
	test_registry_init();
}

int kernel_tests_start_config(struct task_conf *conf)
{
	static bool registered;
	static bool session_started;
	boot_phase_require(BOOT_PHASE_SMP_READY);
	if (!conf || !conf->initialized || !conf->parsed || conf->tests_started) {
		return -1;
	}
	if (conf->purpose != TASK_CONF_TEST && conf->purpose != TASK_CONF_COMPAT) {
		return -1;
	}
	if (session_started) {
		return -1; /* 当前自测调度器一次只支持一个会话。 */
	}
	if (!registered) {
		register_builtin_tests();
		registered = true;
	}
	conf->tests_started = true;
	session_started = true;
	if (task_conf_has_directives_ctx(conf)) {
		selftest_load_payloads_from_files(conf->files, &conf->directives);
		selftest_apply_all(&conf->directives);
	}
	selftest_run();
	return 0;
}

void kernel_tests_start(const struct boot_module_list *modules)
{
	(void)modules;
	(void)kernel_tests_start_config(&task_conf);
}
