# LaOS 顶层构建入口
#
# 用法
# ----
#   make            默认 = make all = make iso（等价于原 build.sh --no-qemu）
#   make kernel     只 build kernel（gen_offsets + GNUmakefile）
#   make user       只 build user/*.elf
#   make module     只 build module/*.{elf,mo}
#   make iso        组 LaOS.iso（隐式依赖 kernel/user/module）
#   make run        iso + qemu
#   make test-x86_64  CI 冒烟：x86_64 构建+QEMU 启动验证（KVM 快）
#   make test-riscv64 CI 冒烟：riscv64 交叉构建+QEMU 启动验证（TCG 慢）
#   make test-all     x86_64 + riscv64 验证
#   make clean      递归 clean + rm -rf build/
#   make distclean  clean + rm .cache compile_commands.json
#   make help       打印目标清单
#
# 产物布局
# --------
#   kernel  →  bin-$(ARCH)/kernel、obj-$(ARCH)/                 （就地产）
#   user    →  user/bin-$(ARCH)/*.elf、user/obj-$(ARCH)/         （就地产）
#   module  →  module/bin-$(ARCH)/*.{elf,mo}、module/obj-$(ARCH)/（就地产）
#   顶层    →  build/iso_root/、build/LaOS.iso、build/serial.log

ARCH ?= x86_64
BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/iso_root
ISO := $(BUILD_DIR)/LaOS.iso
SERIAL_LOG := $(BUILD_DIR)/serial.log
X86_64_TASK_CONF ?= conf/task.conf
X86_64_QEMU_SMP ?= 4
HEADLESS ?= 0
INCLUDE_BAD_MODULE ?= 0

LIMINE_DIR := third_party/limine-c-template/limine-binary
LIMINE := $(LIMINE_DIR)/limine
.PHONY: all kernel user module iso run clean distclean help
.PHONY: test-task-conf-v1 test-x86_64 test-x86_64-lafs test-x86_64-smp-tlb test-x86_64-rollback test-x86_64-negative test-x86_64-sched-stress test-x86_64-rcu-stress test-x86_64-multiuser test-riscv64 test-all
.PHONY: lafs-image

all: iso

kernel: initrd
	cd kernel/arch/$(ARCH) && bash ../../../script/gen_offsets.sh asm_offsets.c
	$(MAKE) -f kernel.mk ARCH=$(ARCH)

# initrd CPIO + embed header (for direct-boot kernel)
initrd: user
	@mkdir -p $(BUILD_DIR)
	python3 script/mkcpio.py -o $(BUILD_DIR)/initrd-$(ARCH).cpio user/bin-$(ARCH)/user.elf:user.elf
	python3 script/embed_bin.py $(BUILD_DIR)/initrd-$(ARCH).cpio kernel/arch/$(ARCH)/initrd_embed.h initrd_cpio_bin

user:
	$(MAKE) -C user ARCH=$(ARCH)

module:
	$(MAKE) -C module ARCH=$(ARCH)

# ISO 组装：把 kernel + user + module + limine + conf 汇入 build/iso_root/，xorriso 打包，limine bios-install 写引导
iso: kernel user module
	mkdir -p $(ISO_ROOT)/boot $(ISO_ROOT)/task $(ISO_ROOT)/conf $(ISO_ROOT)/data
	cp bin-$(ARCH)/kernel                       $(ISO_ROOT)/boot/
	cp user/bin-$(ARCH)/*.elf                   $(ISO_ROOT)/task/
	cp module/bin-$(ARCH)/*.elf                 $(ISO_ROOT)/task/ 2>/dev/null || true
	cp module/bin-$(ARCH)/*.mo                  $(ISO_ROOT)/task/
	cp $(X86_64_TASK_CONF)                      $(ISO_ROOT)/conf/task.conf
	cp $(LIMINE_DIR)/limine-bios.sys            $(ISO_ROOT)/
	cp $(LIMINE_DIR)/limine-bios-cd.bin         $(ISO_ROOT)/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin         $(ISO_ROOT)/
	{ printf '%s\n' \
		'timeout: 3' \
		'/Limine Template' \
		'    protocol: limine' \
		'    path: boot():/boot/kernel' \
		'    module_path: boot():/conf/task.conf' \
		'    module_path: boot():/task/user.elf' \
		'    module_path: boot():/task/thread_bar.elf' \
		'    module_path: boot():/task/module_foo.mo' \
		'    module_path: boot():/task/e1000.mo' \
		'    module_path: boot():/task/test_tlb.mo' \
		'    module_path: boot():/task/test_cpu_alive.mo' \
		'    module_path: boot():/task/test_ipi_delivery.mo' \
		'    module_path: boot():/task/test_fpu_context.mo' \
		'    module_path: boot():/task/module_no_entry.mo' \
		'    module_path: boot():/task/test_init_fail.mo' \
		'    module_path: boot():/task/test_sched_stress.mo'; \
		if [ "$${INCLUDE_BAD_MODULE:-}" = "1" ]; then \
			printf '%s\n' '    module_path: boot():/task/module_bad.mo'; \
		fi; \
	} > $(ISO_ROOT)/limine.conf
	xorriso -as mkisofs -b limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_ROOT) -o $(ISO)
	$(LIMINE) bios-install $(ISO)
	@echo "ISO built: $(ISO)"

# 需要 tap0 已就绪；host 端跑 sudo script/setup_host_net.sh up
# QEMU_ARGS 按架构分发。
ifeq ($(ARCH),x86_64)
QEMU_CMD := qemu-system-x86_64
QEMU_MACHINE := q35
QEMU_NET := -netdev tap,id=u1,ifname=tap0,script=no,downscript=no,poll-us=0 \
	-device e1000,netdev=u1,mac=52:54:00:12:34:56
QEMU_DEBUG := -chardev stdio,id=con0 -device isa-debugcon,iobase=0xe9,chardev=con0
else ifeq ($(ARCH),riscv64)
QEMU_CMD := qemu-system-riscv64
QEMU_MACHINE := virt
QEMU_NET :=
QEMU_DEBUG :=
endif

run: iso
ifeq ($(HEADLESS),1)
	$(QEMU_CMD) -machine $(QEMU_MACHINE) -cdrom $(ISO) \
		-m 2G -smp 4 \
		$(QEMU_NET) \
		-no-reboot -no-shutdown \
		-display none -serial stdio
else
	$(QEMU_CMD) -machine $(QEMU_MACHINE) -cdrom $(ISO) \
		-m 2G -smp 4 \
		$(QEMU_NET) \
		-no-reboot -no-shutdown \
		-serial file:$(SERIAL_LOG) \
		$(QEMU_DEBUG)
endif
# ---- test targets ----
# 成功条件：serial log 含 "LaOS is running"
TEST_TIMEOUT_x86_64 := 15
TEST_TIMEOUT_riscv64 := 120

test-x86_64: iso
	@echo "=== TEST x86_64 (KVM) ==="
	@rm -f $(SERIAL_LOG)
	timeout $(TEST_TIMEOUT_x86_64) $(QEMU_CMD) \
		-machine $(QEMU_MACHINE) -cdrom $(ISO) \
		-m 2G -smp 4 -net none \
		-no-reboot -no-shutdown \
		-display none -serial file:$(SERIAL_LOG) \
		$(QEMU_DEBUG) 2>$(BUILD_DIR)/test-x86_64-qemu.log; \
	status=$$?; test $$status -eq 0 -o $$status -eq 124
	@grep -q "LaOS is running" $(SERIAL_LOG) \
		&& echo "  PASS: x86_64 (boot)" \
		|| (echo "  FAIL: x86_64 boot (serial log: $(SERIAL_LOG))"; exit 1)
	@for test in registry remote_enqueue rcu_publish priority cpu_alive ipi_delivery smp_tlb_remap fpu_context; do \
		if grep -qE "\\[selftest\\] .$$test.* PASSED" $(SERIAL_LOG); then \
			echo "  PASS: x86_64 ($$test)"; \
		else \
			echo "  FAIL: x86_64 $$test (serial log: $(SERIAL_LOG))"; \
			exit 1; \
		fi; \
	done

# ── LaFS disk image ──
LAFS_IMAGE := $(BUILD_DIR)/test-lafs.img

lafs-image:
	python3 script/mkfs_lafs.py -o $(LAFS_IMAGE)

# ── x86_64 LaFS 端到端测试（virtio-blk-pci）──
TEST_TIMEOUT_x86_64_lafs := 20
test-x86_64-lafs: lafs-image iso
	@echo "=== TEST x86_64 LaFS (virtio-blk-pci) ==="
	@rm -f $(SERIAL_LOG)
	timeout $(TEST_TIMEOUT_x86_64_lafs) $(QEMU_CMD) \
		-machine $(QEMU_MACHINE) -cdrom $(ISO) \
		-drive file=$(LAFS_IMAGE),if=none,id=blk0 \
		-device virtio-blk-pci,drive=blk0 \
		-m 2G -smp 4 -net none \
		-no-reboot -no-shutdown \
		-display none -serial file:$(SERIAL_LOG) \
		$(QEMU_DEBUG) 2>$(BUILD_DIR)/test-x86_64-lafs-qemu.log; \
	status=$$?; test $$status -eq 0 -o $$status -eq 124
	@grep -q "LaOS is running" $(SERIAL_LOG) \
		&& echo "  PASS: x86_64 LaFS (boot)" \
		|| (echo "  FAIL: x86_64 LaFS boot (serial log: $(SERIAL_LOG))"; exit 1)
	@grep -q "\\[virtio\\] real virtio-blk LaFS mounted" $(SERIAL_LOG) \
		&& echo "  PASS: x86_64 LaFS (real virtio)" \
		|| (echo "  FAIL: x86_64 LaFS real virtio (serial log: $(SERIAL_LOG))"; exit 1)
	@for test in registry remote_enqueue rcu_publish priority cpu_alive ipi_delivery smp_tlb_remap fpu_context; do \
		if grep -qE "\\[selftest\\] .$$test.* PASSED" $(SERIAL_LOG); then \
			echo "  PASS: x86_64 LaFS ($$test)"; \
		else \
			echo "  FAIL: x86_64 LaFS $$test (serial log: $(SERIAL_LOG))"; \
			exit 1; \
		fi; \
	done

# ── x86_64 SMP TLB shootdown remap stress ──
test-x86_64-smp-tlb:
	@$(MAKE) iso X86_64_TASK_CONF=conf/task-x86_64-tlb.conf
	@echo "=== TEST x86_64 SMP TLB shootdown (smp=$(X86_64_QEMU_SMP)) ==="
	@rm -f $(SERIAL_LOG)
	timeout $(TEST_TIMEOUT_x86_64) $(QEMU_CMD) \
		-machine $(QEMU_MACHINE) -cdrom $(ISO) \
		-m 2G -smp $(X86_64_QEMU_SMP) -net none \
		-no-reboot -no-shutdown \
		-display none -serial file:$(SERIAL_LOG) \
		$(QEMU_DEBUG) 2>$(BUILD_DIR)/test-x86_64-smp-tlb-qemu.log; \
	status=$$?; test $$status -eq 0 -o $$status -eq 124
	@for test in registry rcu_publish cpu_alive ipi_delivery smp_tlb_remap; do \
		if grep -qE "\\[selftest\\] .$$test.* PASSED" $(SERIAL_LOG); then \
			echo "  PASS: x86_64 smp-tlb ($$test)"; \
		else \
			echo "  FAIL: x86_64 smp-tlb $$test (serial log: $(SERIAL_LOG))"; \
			exit 1; \
		fi; \
	done

test-x86_64-rollback:
	@$(MAKE) iso X86_64_TASK_CONF=conf/task-x86_64-rollback.conf
	@echo "=== TEST x86_64 module rollback ==="
	@rm -f $(SERIAL_LOG)
	timeout $(TEST_TIMEOUT_x86_64) $(QEMU_CMD) \
		-machine $(QEMU_MACHINE) -cdrom $(ISO) \
		-m 2G -smp 1 -net none \
		-no-reboot -no-shutdown \
		-display none -serial file:$(SERIAL_LOG) \
		$(QEMU_DEBUG) 2>$(BUILD_DIR)/test-x86_64-rollback-qemu.log; \
	status=$$?; test $$status -eq 0 -o $$status -eq 124
	@if grep -aFq '[ module] module_alloc rollback:' $(SERIAL_LOG); then \
		echo "  PASS: x86_64 rollback (module_alloc recovered)"; \
	else \
		echo "  FAIL: x86_64 rollback (no rollback; log: $(SERIAL_LOG))"; exit 1; \
	fi
	if grep -qE "\\[selftest\\] .registry.* PASSED" $(SERIAL_LOG); then \
		echo "  PASS: x86_64 rollback (registry)"; \
	else \
		echo "  FAIL: x86_64 rollback registry (serial log: $(SERIAL_LOG))"; exit 1; \
	fi

test-x86_64-negative:
	@$(MAKE) iso X86_64_TASK_CONF=conf/task-x86_64-negative.conf INCLUDE_BAD_MODULE=1
	@echo "=== TEST x86_64 module negative ==="
	@rm -f $(SERIAL_LOG)
	timeout $(TEST_TIMEOUT_x86_64) $(QEMU_CMD) \
		-machine $(QEMU_MACHINE) -cdrom $(ISO) \
		-m 2G -smp 1 -net none \
		-no-reboot -no-shutdown \
		-display none -serial file:$(SERIAL_LOG) \
		$(QEMU_DEBUG) 2>$(BUILD_DIR)/test-x86_64-negative-qemu.log; \
	status=$$?; test $$status -eq 0 -o $$status -eq 124
	@if grep -aFq 'unresolved module symbol before allocation: laos_missing_symbol_for_negative_test' $(SERIAL_LOG); then \
		echo "  PASS: x86_64 negative (bad module rejected)"; \
	else \
		echo "  FAIL: x86_64 negative (bad module not rejected; log: $(SERIAL_LOG))"; exit 1; \
	fi
	@if grep -aFq 'selftest_init failed (ret=-1)' $(SERIAL_LOG); then \
		echo "  PASS: x86_64 negative (init_fail cancel)"; \
	else \
		echo "  FAIL: x86_64 negative init_fail (serial log: $(SERIAL_LOG))"; exit 1; \
	fi

test-x86_64-sched-stress:
	@$(MAKE) iso X86_64_TASK_CONF=conf/task-x86_64-stress.conf
	@echo "=== TEST x86_64 scheduler stress (smp=$(X86_64_QEMU_SMP)) ==="
	@rm -f $(SERIAL_LOG)
	timeout $(TEST_TIMEOUT_x86_64) $(QEMU_CMD) \
		-machine $(QEMU_MACHINE) -cdrom $(ISO) \
		-m 2G -smp $(X86_64_QEMU_SMP) -net none \
		-no-reboot -no-shutdown \
		-display none -serial file:$(SERIAL_LOG) \
		$(QEMU_DEBUG) 2>$(BUILD_DIR)/test-x86_64-stress-qemu.log; \
	status=$$?; test $$status -eq 0 -o $$status -eq 124
	@for test in registry cpu_alive ipi_delivery sched_stress; do \
		if grep -qE "\\[selftest\\] .$$test.* PASSED" $(SERIAL_LOG); then \
			echo "  PASS: x86_64 stress ($$test)"; \
		else \
			echo "  FAIL: x86_64 stress $$test (serial log: $(SERIAL_LOG))"; \
			exit 1; \
		fi; \
	done

TEST_TIMEOUT_x86_64_RCU_STRESS := 20
test-x86_64-rcu-stress:
	@$(MAKE) iso X86_64_TASK_CONF=conf/task-x86_64-rcu-stress.conf
	@QEMU_SYSTEM_X86_64="$(QEMU_CMD)" \
		QEMU_MACHINE="$(QEMU_MACHINE)" \
		X86_64_QEMU_SMP="$(X86_64_QEMU_SMP)" \
		X86_64_RCU_STRESS_TIMEOUT="$(TEST_TIMEOUT_x86_64_RCU_STRESS)" \
		bash script/test_x86_64_rcu_stress.sh

test-x86_64-multiuser:
	@$(MAKE) iso X86_64_TASK_CONF=conf/task-x86_64-multiuser.conf
	@echo "=== TEST x86_64 multi-user ==="
	@rm -f $(SERIAL_LOG)
	timeout $(TEST_TIMEOUT_x86_64) $(QEMU_CMD) \
		-machine $(QEMU_MACHINE) -cdrom $(ISO) \
		-m 2G -smp 1 -net none \
		-no-reboot -no-shutdown \
		-display none -serial file:$(SERIAL_LOG) \
		$(QEMU_DEBUG) 2>$(BUILD_DIR)/test-x86_64-multiuser-qemu.log; \
	status=$$?; test $$status -eq 0 -o $$status -eq 124
	@count=$$(grep -acF 'M3c: ELF -> EL0 -> syscall write -> exit, DONE' $(SERIAL_LOG) || true); \
	if [ "$$count" -ge 3 ]; then \
		echo "  PASS: x86_64 multiuser ($$count exits)"; \
	else \
		echo "  FAIL: x86_64 multiuser ($$count exits, expected >= 3; log: $(SERIAL_LOG))"; exit 1; \
	fi

test-task-conf-v1:
	bash script/check_task_conf_v1.sh \
		conf/task.conf \
		conf/task-x86_64-rollback.conf \
		conf/task-x86_64-negative.conf \
		conf/task-x86_64-stress.conf \
		conf/task-x86_64-rcu-stress.conf \
		conf/task-x86_64-multiuser.conf \
		conf/task-conf-v1-sample.conf

test-riscv64:
	@if [ ! -d kernel/arch/riscv64 ]; then \
		echo "=== SKIP riscv64: no arch directory (port not started) ==="; \
	else \
		$(MAKE) iso ARCH=riscv64 BUILD_DIR=$(BUILD_DIR)/test-riscv64 && \
		echo "=== TEST riscv64 (TCG) ===" && \
		timeout $(TEST_TIMEOUT_riscv64) qemu-system-riscv64 \
			-machine virt -cpu rv64 \
			-cdrom $(BUILD_DIR)/test-riscv64/LaOS.iso \
			-m 2G -smp 4 \
			-no-reboot -no-shutdown \
			-display none -serial file:$(BUILD_DIR)/test-riscv64/serial.log \
			2>/dev/null; true && \
		if grep -q "LaOS is running" $(BUILD_DIR)/test-riscv64/serial.log; then \
			echo "  PASS: riscv64"; \
		else \
			echo "  FAIL: riscv64 (serial log: $(BUILD_DIR)/test-riscv64/serial.log)"; \
			exit 1; \
		fi; \
	fi

test-all: test-task-conf-v1 test-x86_64 test-x86_64-lafs test-x86_64-rollback test-x86_64-negative test-x86_64-sched-stress test-x86_64-multiuser test-riscv64
	@echo "=== All architecture tests complete ==="

clean:
	-$(MAKE) -f kernel.mk clean
	-$(MAKE) -C user clean
	-$(MAKE) -C module clean
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -rf .cache compile_commands.json

help:
	@echo "LaOS build targets:"
	@echo "  make              = make all = make iso"
	@echo "  make kernel       build kernel only"
	@echo "  make user         build user ELFs only"
	@echo "  make module       build kernel modules only"
	@echo "  make iso          build $(ISO)"
	@echo "  make run          build iso + launch qemu (host needs tap0)"
	@echo "  make run HEADLESS=1  same as run, but -display none -serial stdio"
	@echo "  make test-task-conf-v1 validate task.conf DSL v1 fixtures"
	@echo ""
	@echo "  make test-x86_64   CI smoke: build + QEMU boot check (KVM, ~5s)"
	@echo "  make test-x86_64-lafs  x86_64 LaFS via virtio-blk-pci (disk image + mount + selftests)"
	@echo "  make test-x86_64-smp-tlb  x86_64 SMP TLB shootdown remap stress (X86_64_QEMU_SMP=4 default)"
	@echo "  make test-x86_64-rollback  x86_64 module loading transaction rollback"
	@echo "  make test-x86_64-negative  x86_64 reject bad module + selftest init fail"
	@echo "  make test-x86_64-sched-stress  x86_64 scheduler stress (4 CPUs)"
	@echo "  make test-x86_64-rcu-stress  x86_64 configurable preemptible-RCU stress"
	@echo "  make test-x86_64-multiuser  x86_64 multi-user tasks"
	@echo "  make test-riscv64  CI smoke: cross-build + QEMU boot check (TCG, ~60s)"
	@echo "  make test-all      run all available tests in order"
	@echo ""
	@echo "  make clean        wipe build artifacts (build/ + bin-*/ + obj-*/)"
	@echo "  make distclean     clean + .cache + compile_commands.json"
	@echo ""
	@echo "Variables:"
	@echo "  ARCH=$(ARCH)  (supported: x86_64, riscv64)"
