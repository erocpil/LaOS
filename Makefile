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
#   make test-arm64   当前工作树未包含 aarch64 代码，目标会明确跳过
#   make test-riscv64 当前工作树未包含 riscv64 代码，目标会明确跳过
#   make test-all     运行 x86_64 回归；缺失的架构目标会跳过
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
ARM64_TASK_CONF ?= conf/task.conf
ARM64_INCLUDE_BAD_MODULE ?= 0
ARM64_QEMU_SMP ?= 2
ARM64_EXPECT_SMP_TASK_CPUS ?=
X86_64_TASK_CONF ?= conf/task.conf
X86_64_QEMU_SMP ?= 4
HEADLESS ?= 0
INCLUDE_BAD_MODULE ?= 0

LIMINE_DIR := third_party/limine-c-template/limine-binary
LIMINE := $(LIMINE_DIR)/limine
.PHONY: all kernel kernel-limine iso-limine-arm64 user module iso run clean distclean help
.PHONY: test-task-conf-v1 test-x86_64 test-x86_64-lafs test-x86_64-smp-tlb test-x86_64-rollback test-x86_64-negative test-x86_64-sched-stress test-x86_64-rcu-stress test-x86_64-multiuser test-arm64 test-arm64-limine test-arm64-limine-negative test-arm64-limine-rollback test-arm64-limine-smp-park test-arm64-limine-smp-tlb test-arm64-limine-fpu test-arm64-limine-sched-stress test-arm64-limine-multiuser test-arm64-lafs test-arm64-lafs-negative test-riscv64 test-all
.PHONY: lafs-image

all: iso

kernel: initrd
ifeq ($(ARCH),aarch64)
	cd kernel/arch/aarch64 && touch asm_offsets.c && bash ../../../script/gen_offsets.sh asm_offsets.c 2>/dev/null; true
	$(MAKE) -f kernel.mk ARCH=$(ARCH) TOOLCHAIN=aarch64-linux-gnu e1000_objs
	$(MAKE) -f kernel.mk ARCH=$(ARCH) TOOLCHAIN=aarch64-linux-gnu
else
	cd kernel/arch/$(ARCH) && bash ../../../script/gen_offsets.sh asm_offsets.c
	$(MAKE) -f kernel.mk ARCH=$(ARCH)
endif

# initrd CPIO + embed header (for direct-boot kernel)
initrd: user
	@mkdir -p $(BUILD_DIR)
	python3 script/mkcpio.py -o $(BUILD_DIR)/initrd-$(ARCH).cpio user/bin-$(ARCH)/user.elf:user.elf
	python3 script/embed_bin.py $(BUILD_DIR)/initrd-$(ARCH).cpio kernel/arch/$(ARCH)/initrd_embed.h initrd_cpio_bin

# Experimental AArch64 Limine image.  The normal `kernel` target remains the
# QEMU -kernel direct-boot image used by the regression tests.
kernel-limine: initrd
	@test "$(ARCH)" = "aarch64" || (echo "kernel-limine requires ARCH=aarch64"; exit 1)
	$(MAKE) -f kernel.mk ARCH=aarch64 TOOLCHAIN=aarch64-linux-gnu \
		LIMINE_AARCH64=1 OUTPUT=kernel-limine e1000_objs
	$(MAKE) -f kernel.mk ARCH=aarch64 TOOLCHAIN=aarch64-linux-gnu \
		LIMINE_AARCH64=1 OUTPUT=kernel-limine

# Build an AArch64 UEFI/Limine ISO around the experimental high-half image.
# This target is intentionally separate from `iso`, whose ARM64 path is not
# yet a Limine boot path.
iso-limine-arm64:
	@$(MAKE) kernel-limine ARCH=aarch64
	@$(MAKE) user ARCH=aarch64
	@$(MAKE) module ARCH=aarch64 TOOLCHAIN=aarch64-linux-gnu
	@set -e; \
	root=$(BUILD_DIR)/iso-limine-arm64; \
	rm -rf "$$root"; \
	mkdir -p "$$root/boot/limine" "$$root/EFI/BOOT" "$$root/task" "$$root/conf"; \
	cp bin-aarch64/kernel-limine "$$root/boot/kernel"; \
	cp third_party/limine-c-template/limine-binary/BOOTAA64.EFI "$$root/EFI/BOOT/"; \
	cp third_party/limine-c-template/limine-binary/limine-uefi-cd.bin "$$root/boot/limine/"; \
	cp $(ARM64_TASK_CONF) "$$root/conf/task.conf"; \
	cp user/bin-aarch64/*.elf module/bin-aarch64/*.mo "$$root/task/"; \
	if [ "$(ARM64_INCLUDE_BAD_MODULE)" != "1" ]; then rm -f "$$root/task/module_bad.mo"; fi; \
	{ printf '%s\n' 'timeout: 1' 'serial: yes' 'verbose: yes' '/LaOS ARM64 Limine' \
	  '    protocol: limine' '    path: boot():/boot/kernel' \
	  '    module_path: boot():/conf/task.conf' \
	  '    module_path: boot():/task/user.elf' \
	  '    module_path: boot():/task/module_foo.mo' \
	  '    module_path: boot():/task/module_abi.mo' \
	  '    module_path: boot():/task/e1000.mo' \
	  '    module_path: boot():/task/module_no_entry.mo' \
	  '    module_path: boot():/task/test_tlb.mo' \
	  '    module_path: boot():/task/test_cpu_alive.mo' \
	  '    module_path: boot():/task/test_ipi_delivery.mo' \
	  '    module_path: boot():/task/module_smp_probe.mo' \
	  '    module_path: boot():/task/test_init_fail.mo' \
	  '    module_path: boot():/task/test_fpu_arm64.mo' \
	  '    module_path: boot():/task/test_sched_stress.mo'; \
	  if [ "$(ARM64_INCLUDE_BAD_MODULE)" = "1" ]; then \
	    printf '%s\n' '    module_path: boot():/task/module_bad.mo'; \
	  fi; } > "$$root/boot/limine/limine.conf"; \
	xorriso -as mkisofs -R -r -J -hfsplus -apm-block-size 2048 \
	  --efi-boot boot/limine/limine-uefi-cd.bin \
	  -efi-boot-part --efi-boot-image --protective-msdos-label \
	  "$$root" \
	  -o $(BUILD_DIR)/LaOS-arm64-limine.iso; \
	echo "ISO built: $(BUILD_DIR)/LaOS-arm64-limine.iso"

user:
	$(MAKE) -C user ARCH=$(ARCH)

module:
ifeq ($(ARCH),aarch64)
	$(MAKE) -C module ARCH=$(ARCH) TOOLCHAIN=aarch64-linux-gnu
else
	$(MAKE) -C module ARCH=$(ARCH)
endif

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
# QEMU_ARGS 按架构分发：x86_64 用 q35+e1000，aarch64/riscv64 用 virt（无网络）
ifeq ($(ARCH),x86_64)
QEMU_CMD := qemu-system-x86_64
QEMU_MACHINE := q35
QEMU_NET := -netdev tap,id=u1,ifname=tap0,script=no,downscript=no,poll-us=0 \
	-device e1000,netdev=u1,mac=52:54:00:12:34:56
QEMU_DEBUG := -chardev stdio,id=con0 -device isa-debugcon,iobase=0xe9,chardev=con0
else ifeq ($(ARCH),aarch64)
QEMU_CMD := qemu-system-aarch64
QEMU_MACHINE := virt,gic-version=3,highmem-ecam=off
QEMU_NET := -netdev tap,id=u1,ifname=tap0,script=no,downscript=no,poll-us=0 \
	-device e1000,netdev=u1,mac=52:54:00:12:34:56
QEMU_DEBUG :=
else ifeq ($(ARCH),riscv64)
QEMU_CMD := qemu-system-riscv64
QEMU_MACHINE := virt
QEMU_NET :=
QEMU_DEBUG :=
endif

ifeq ($(ARCH),aarch64)
run: kernel
	$(QEMU_CMD) -machine $(QEMU_MACHINE) -cpu cortex-a57 \
		-kernel bin-aarch64/kernel -m 512M -smp 4 \
		$(QEMU_NET) -no-reboot -no-shutdown \
		-display none -serial stdio
else
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
endif

# ---- test targets: x86_64 CI + 缺失架构占位目标 ----
# 当前工作树仅包含 x86_64。aarch64/riscv64 目标保留为移植入口，
# 在相应架构目录缺失时明确跳过，不能作为验证结果。
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

test-arm64:
	@if [ ! -d kernel/arch/aarch64 ]; then \
		echo "=== SKIP aarch64: no arch directory (port not started) ==="; \
	else \
		$(MAKE) kernel ARCH=aarch64 && \
		bash script/test_arm64_direct.sh; \
	fi

test-arm64-limine: iso-limine-arm64
	bash script/check_arm64_module_abi.sh
	ARM64_QEMU_SMP=2 ARM64_EXPECT_REMOTE_ENQUEUE=1 ARM64_EXPECT_RCU=1 \
	bash script/test_arm64_limine.sh

# ── LaFS disk image ──
LAFS_IMAGE := $(BUILD_DIR)/test-lafs.img

lafs-image:
	python3 script/mkfs_lafs.py -o $(LAFS_IMAGE)

# ── ARM64 LaFS 回归测试 ──
test-arm64-lafs: lafs-image
	@if [ ! -d kernel/arch/aarch64 ]; then \
		echo "=== SKIP aarch64-lafs: no arch directory ==="; \
	else \
		$(MAKE) kernel ARCH=aarch64 && \
		bash script/test_arm64_lafs.sh; \
	fi

# ── ARM64 LaFS 负向测试（bad superblock）──
test-arm64-lafs-negative:
	@if [ ! -d kernel/arch/aarch64 ]; then \
		echo "=== SKIP aarch64-lafs-negative: no arch directory ==="; \
	else \
		$(MAKE) kernel ARCH=aarch64 && \
		bash script/test_arm64_lafs_negative.sh; \
	fi

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
		conf/task-arm64.conf \
		conf/task-arm64-negative.conf \
		conf/task-arm64-rollback.conf \
		conf/task-arm64-tlb.conf \
		conf/task-arm64-fpu.conf \
		conf/task-x86_64-rollback.conf \
		conf/task-x86_64-negative.conf \
		conf/task-x86_64-stress.conf \
		conf/task-x86_64-rcu-stress.conf \
		conf/task-x86_64-multiuser.conf \
		conf/task-conf-v1-sample.conf

test-arm64-limine-negative:
	$(MAKE) iso-limine-arm64 ARM64_TASK_CONF=conf/task-arm64-negative.conf ARM64_INCLUDE_BAD_MODULE=1
	bash script/check_arm64_module_abi.sh
	ARM64_EXPECT_FAILED_MODULE=module_bad.mo bash script/test_arm64_limine.sh

test-arm64-limine-rollback:
	$(MAKE) iso-limine-arm64 ARM64_TASK_CONF=conf/task-arm64-rollback.conf
	bash script/check_arm64_module_abi.sh
	ARM64_EXPECT_FAILED_MODULE=module_no_entry.mo \
	ARM64_EXPECT_MODULE_ROLLBACK=1 \
	bash script/test_arm64_limine.sh

test-arm64-limine-smp-park: iso-limine-arm64
	bash script/check_arm64_module_abi.sh
	ARM64_QEMU_SMP=$(ARM64_QEMU_SMP) \
	ARM64_EXPECT_PARKED_APS=$$(($(ARM64_QEMU_SMP) - 1)) \
	ARM64_EXPECT_GIC_APS=$$(($(ARM64_QEMU_SMP) - 1)) \
	ARM64_EXPECT_ONLINE_APS=$$(($(ARM64_QEMU_SMP) - 1)) \
	ARM64_EXPECT_SMP_TASK_CPUS="$(ARM64_EXPECT_SMP_TASK_CPUS)" \
	bash script/test_arm64_limine.sh

test-arm64-limine-smp-tlb:
	$(MAKE) iso-limine-arm64 ARM64_TASK_CONF=conf/task-arm64-tlb.conf
	bash script/check_arm64_module_abi.sh
	ARM64_QEMU_SMP=$(ARM64_QEMU_SMP) \
	ARM64_EXPECT_PARKED_APS=$$(($(ARM64_QEMU_SMP) - 1)) \
	ARM64_EXPECT_GIC_APS=$$(($(ARM64_QEMU_SMP) - 1)) \
	ARM64_EXPECT_SGI_ACKS=$$(($(ARM64_QEMU_SMP) - 1)) \
	ARM64_EXPECT_TLB_REMAP=$$(($(ARM64_QEMU_SMP) - 1)) \
	ARM64_EXPECT_TLB_REMAP_ROUNDS=50 \
	ARM64_EXPECT_ONLINE_APS=$$(($(ARM64_QEMU_SMP) - 1)) \
	ARM64_EXPECT_SMP_TASK_CPUS="$(ARM64_EXPECT_SMP_TASK_CPUS)" \
	bash script/test_arm64_limine.sh

test-arm64-limine-fpu:
	$(MAKE) iso-limine-arm64 ARM64_TASK_CONF=conf/task-arm64-fpu.conf
	bash script/check_arm64_module_abi.sh
	ARM64_EXPECT_FPU=1 bash script/test_arm64_limine.sh

test-arm64-limine-sched-stress:
	$(MAKE) iso-limine-arm64 ARM64_TASK_CONF=conf/task-arm64-stress.conf
	bash script/check_arm64_module_abi.sh
	ARM64_QEMU_SMP=4 ARM64_EXPECT_STRESS=1 bash script/test_arm64_limine.sh

test-arm64-limine-multiuser:
	$(MAKE) iso-limine-arm64 ARM64_TASK_CONF=conf/task-arm64-multiuser.conf
	bash script/check_arm64_module_abi.sh
	ARM64_EXPECT_MULTIUSER=1 bash script/test_arm64_limine.sh

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

test-all: test-task-conf-v1 test-x86_64 test-x86_64-lafs test-x86_64-rollback test-x86_64-negative test-x86_64-sched-stress test-x86_64-multiuser test-arm64 test-arm64-lafs test-riscv64
	@echo "=== All architecture tests complete ==="

clean:
	-$(MAKE) -f kernel.mk clean
	-$(MAKE) -f kernel.mk clean ARCH=aarch64 TOOLCHAIN=aarch64-linux-gnu
	-$(MAKE) -C user clean
	-$(MAKE) -C module clean
	-$(MAKE) -C module clean ARCH=aarch64 TOOLCHAIN=aarch64-linux-gnu
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
	@echo "  make test-arm64    placeholder: skipped while kernel/arch/aarch64 is absent"
	@echo "  make test-arm64-limine               Limine UEFI: module ABI + EL0 task chain"
	@echo "  make test-arm64-limine-negative      Limine UEFI: reject bad module, then continue"
	@echo "  make test-arm64-limine-rollback      Limine UEFI: module init failure rollback"
	@echo "  make test-arm64-limine-smp-park      Limine UEFI: SMP AP parking"
	@echo "  make test-arm64-limine-smp-tlb       Limine UEFI: SMP TLB shootdown"
	@echo "  make test-arm64-limine-fpu           Limine UEFI: FPU/SIMD selftest"
	@echo "  make test-arm64-limine-sched-stress  Limine UEFI: scheduler stress (4 CPUs)"
	@echo "  make test-arm64-limine-multiuser     Limine UEFI: multi-user tasks"
	@echo "  make test-riscv64  placeholder: skipped while kernel/arch/riscv64 is absent"
	@echo "  make test-all      run x86_64 regressions; absent architecture targets skip"
	@echo ""
	@echo "  make clean        wipe build artifacts (build/ + bin-*/ + obj-*/)"
	@echo "  make distclean     clean + .cache + compile_commands.json"
	@echo ""
	@echo "Variables:"
	@echo "  ARCH=$(ARCH)  (implemented here: x86_64; aarch64/riscv64 are placeholders)"
