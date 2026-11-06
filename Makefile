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
#   make test-arm64   CI 冒烟：aarch64 交叉构建+QEMU 启动验证（TCG 慢）
#   make test-riscv64 CI 冒烟：riscv64 交叉构建+QEMU 启动验证（TCG 慢）
#   make test-all     三架构全跑（x86_64→aarch64→riscv64）
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
NM ?= nm
HOST_OS := $(shell uname -s)
BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/iso_root
ISO := $(BUILD_DIR)/LaOS.iso
SERIAL_LOG := $(BUILD_DIR)/serial.log
ARM64_TASK_CONF ?= conf/task.conf
ARM64_INCLUDE_BAD_MODULE ?= 0
ARM64_QEMU_SMP ?= 2
ARM64_EXPECT_SMP_TASK_CPUS ?=
ARM64_TOOLCHAIN ?= aarch64-linux-gnu
ARM64_NM ?= $(if $(filter llvm,$(ARM64_TOOLCHAIN)),llvm-nm,$(ARM64_TOOLCHAIN)-nm)
X86_64_TASK_CONF ?= conf/task.conf
X86_64_BOOT_MODE ?= base
X86_64_TEST_CONF ?= conf/test.conf
X86_64_INITRD ?= $(BUILD_DIR)/initrd-$(ARCH).cpio
X86_64_QEMU_SMP ?= 4
HEADLESS ?= 0
INCLUDE_BAD_MODULE ?= 0

LIMINE_DIR := third_party/limine-c-template/limine-binary
LIMINE := $(LIMINE_DIR)/limine
.PHONY: all kernel kernel-limine iso-limine-arm64 user module iso run clean distclean help
.PHONY: test-task-conf-v1 test-x86_64 test-x86_64-lafs test-x86_64-smp-tlb test-x86_64-rollback test-x86_64-negative test-x86_64-sched-stress test-x86_64-rcu-stress test-x86_64-multiuser test-x86_64-boot-payload test-arm64 test-arm64-limine test-arm64-limine-negative test-arm64-limine-rollback test-arm64-limine-smp-park test-arm64-limine-smp-tlb test-arm64-limine-fpu test-arm64-limine-sched-stress test-arm64-limine-multiuser test-arm64-lafs test-arm64-lafs-negative test-riscv64 test-all
.PHONY: lafs-image

all: iso

ifeq ($(ARCH),x86_64)
kernel:
else
kernel: embedded-initrd
endif
ifeq ($(ARCH),aarch64)
	cd kernel/arch/aarch64 && touch asm_offsets.c && OFFSET_CC=$(if $(filter llvm,$(ARM64_TOOLCHAIN)),clang,$(ARM64_TOOLCHAIN)-gcc) OFFSET_TARGET=$(if $(filter llvm,$(ARM64_TOOLCHAIN)),aarch64-none-elf,) bash ../../../script/gen_offsets.sh asm_offsets.c 2>/dev/null; true
	$(MAKE) -f kernel.mk ARCH=$(ARCH) TOOLCHAIN=$(ARM64_TOOLCHAIN) e1000_objs
	$(MAKE) -f kernel.mk ARCH=$(ARCH) TOOLCHAIN=$(ARM64_TOOLCHAIN) NM=$(ARM64_NM)
else
	cd kernel/arch/$(ARCH) && bash ../../../script/gen_offsets.sh asm_offsets.c
	$(MAKE) -f kernel.mk ARCH=$(ARCH)
endif

# Legacy embedded payload, retained until the external boot handoff is wired.
embedded-initrd: user
	@mkdir -p $(BUILD_DIR)
	python3 script/mkcpio.py -o $(BUILD_DIR)/initrd-embedded-$(ARCH).cpio user/bin-$(ARCH)/user.elf:user.elf
	python3 script/embed_bin.py $(BUILD_DIR)/initrd-embedded-$(ARCH).cpio kernel/arch/$(ARCH)/initrd_embed.h initrd_cpio_bin

# Experimental AArch64 Limine image.  The normal `kernel` target remains the
# QEMU -kernel direct-boot image used by the regression tests.
kernel-limine: embedded-initrd
	@test "$(ARCH)" = "aarch64" || (echo "kernel-limine requires ARCH=aarch64"; exit 1)
	cd kernel/arch/aarch64 && OFFSET_CC=$(if $(filter llvm,$(ARM64_TOOLCHAIN)),clang,$(ARM64_TOOLCHAIN)-gcc) OFFSET_TARGET=$(if $(filter llvm,$(ARM64_TOOLCHAIN)),aarch64-none-elf,) bash ../../../script/gen_offsets.sh asm_offsets.c
	$(MAKE) -f kernel.mk ARCH=aarch64 TOOLCHAIN=$(ARM64_TOOLCHAIN) \
		LIMINE_AARCH64=1 CPPFLAGS=-DLIMINE_AARCH64 OUTPUT=kernel-limine e1000_objs
	$(MAKE) -f kernel.mk ARCH=aarch64 TOOLCHAIN=$(ARM64_TOOLCHAIN) \
		LIMINE_AARCH64=1 CPPFLAGS=-DLIMINE_AARCH64 OUTPUT=kernel-limine NM=$(ARM64_NM)

# Build an AArch64 UEFI/Limine ISO around the experimental high-half image.
# This target is intentionally separate from `iso`, whose ARM64 path is not
# yet a Limine boot path.
iso-limine-arm64:
	@$(MAKE) kernel-limine ARCH=aarch64
	@$(MAKE) user ARCH=aarch64 TOOLCHAIN=$(ARM64_TOOLCHAIN)
	@$(MAKE) module ARCH=aarch64 TOOLCHAIN=$(ARM64_TOOLCHAIN)
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
	  '    module_path: boot():/task/test_priority.mo' \
	  '    module_path: boot():/task/test_remote_enqueue.mo' \
	  '    module_path: boot():/task/test_rcu_publish.mo' \
	  '    module_path: boot():/task/test_rcu_stress.mo' \
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
ifeq ($(ARCH),aarch64)
	$(MAKE) -C user ARCH=$(ARCH) TOOLCHAIN=$(ARM64_TOOLCHAIN)
else
	$(MAKE) -C user ARCH=$(ARCH)
endif

module:
ifeq ($(ARCH),aarch64)
	$(MAKE) -C module ARCH=$(ARCH) TOOLCHAIN=$(ARM64_TOOLCHAIN)
else
	$(MAKE) -C module ARCH=$(ARCH)
endif

# ISO 组装：把 kernel + user + module + limine + conf 汇入 build/iso_root/，xorriso 打包，limine bios-install 写引导
ifneq ($(filter base test,$(X86_64_BOOT_MODE)),)
iso-payload: kernel initrd $(if $(filter test,$(X86_64_BOOT_MODE)),module)
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot
	cp bin-$(ARCH)/kernel $(ISO_ROOT)/boot/
	cp $(X86_64_INITRD) $(ISO_ROOT)/boot/initrd.cpio
	{ printf '%s\n' 'timeout: 1' '/LaOS base' '    protocol: limine' '    path: boot():/boot/kernel' '    cmdline: boot=$(X86_64_BOOT_MODE)' '    module_path: boot():/boot/initrd.cpio'; } > $(ISO_ROOT)/limine.conf
ifeq ($(X86_64_BOOT_MODE),test)
	mkdir -p $(ISO_ROOT)/task $(ISO_ROOT)/conf
	cp $(X86_64_TEST_CONF) $(ISO_ROOT)/conf/test.conf
	cp user/bin-$(ARCH)/user.elf $(ISO_ROOT)/task/
	printf '%s\n' '    module_path: boot():/conf/test.conf' '    module_path: boot():/task/user.elf' >> $(ISO_ROOT)/limine.conf
	@set -e; for file in test_priority.mo test_remote_enqueue.mo test_rcu_publish.mo test_rcu_stress.mo thread_bar.elf test_task_params.elf module_foo.mo module_abi.mo e1000.mo test_tlb.mo test_cpu_alive.mo test_ipi_delivery.mo test_fpu_context.mo module_no_entry.mo test_init_fail.mo test_sched_stress.mo; do \
		cp module/bin-$(ARCH)/"$$file" $(ISO_ROOT)/task/; \
		printf '    module_path: boot():/task/%s\n' "$$file" >> $(ISO_ROOT)/limine.conf; \
	done
	@set -e; if [ "$(INCLUDE_BAD_MODULE)" = 1 ]; then cp module/bin-$(ARCH)/module_bad.mo $(ISO_ROOT)/task/; printf '%s\n' '    module_path: boot():/task/module_bad.mo' >> $(ISO_ROOT)/limine.conf; fi
endif
else ifeq ($(X86_64_BOOT_MODE),compat)
iso-payload: kernel user module
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot $(ISO_ROOT)/task $(ISO_ROOT)/conf $(ISO_ROOT)/data
	cp bin-$(ARCH)/kernel                       $(ISO_ROOT)/boot/
	cp user/bin-$(ARCH)/*.elf                   $(ISO_ROOT)/task/
	cp module/bin-$(ARCH)/*.elf                 $(ISO_ROOT)/task/ 2>/dev/null || true
	cp module/bin-$(ARCH)/*.mo                  $(ISO_ROOT)/task/
	cp $(X86_64_TASK_CONF)                      $(ISO_ROOT)/conf/task.conf
	{ printf '%s\n' \
		'timeout: 3' \
		'/Limine Template' \
		'    protocol: limine' \
		'    path: boot():/boot/kernel' \
		'    cmdline: boot=compat' \
		'    module_path: boot():/conf/task.conf' \
		'    module_path: boot():/task/user.elf' \
		'    module_path: boot():/task/test_priority.mo' \
		'    module_path: boot():/task/test_remote_enqueue.mo' \
		'    module_path: boot():/task/test_rcu_publish.mo' \
		'    module_path: boot():/task/test_rcu_stress.mo' \
		'    module_path: boot():/task/thread_bar.elf' \
		'    module_path: boot():/task/test_task_params.elf' \
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
else
$(error X86_64_BOOT_MODE must be base, test or compat)
endif

.PHONY: iso-payload
iso: iso-payload
	cp $(LIMINE_DIR)/limine-bios.sys $(ISO_ROOT)/
	cp $(LIMINE_DIR)/limine-bios-cd.bin $(ISO_ROOT)/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/
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
X86_64_QEMU_ACCEL ?= tcg
QEMU_ACCEL_ARGS := -accel $(X86_64_QEMU_ACCEL)
QEMU_NET := -netdev tap,id=u1,ifname=tap0,script=no,downscript=no,poll-us=0 \
	-device e1000,netdev=u1,mac=52:54:00:12:34:56
QEMU_DEBUG := -chardev stdio,id=con0 -device isa-debugcon,iobase=0xe9,chardev=con0
else ifeq ($(ARCH),aarch64)
QEMU_CMD := qemu-system-aarch64
QEMU_MACHINE := virt,gic-version=3,highmem-ecam=off
QEMU_ACCEL_ARGS :=
ifeq ($(HOST_OS),Darwin)
QEMU_NET ?= -netdev vmnet-shared,id=u1 \
	-device e1000,netdev=u1,mac=52:54:00:12:34:56
else
QEMU_NET ?= -netdev tap,id=u1,ifname=tap0,script=no,downscript=no,poll-us=0 \
	-device e1000,netdev=u1,mac=52:54:00:12:34:56
endif
QEMU_DEBUG :=
else ifeq ($(ARCH),riscv64)
QEMU_CMD := qemu-system-riscv64
QEMU_MACHINE := virt
QEMU_ACCEL_ARGS :=
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

# ---- test targets: 三架构 CI 冒烟 ----
# 三层验证模型：
#   第一层 x86_64 (KVM 或 TCG) → 每次改动
#   第二层 aarch64 (TCG，成功标志出现后立即退出) → 每次 git commit 前
#   第三层 riscv64 (TCG，30-60s) → 架构移植 session 中
# 成功条件：serial log 含 "LaOS is running"
ifeq ($(X86_64_QEMU_ACCEL),kvm)
TEST_TIMEOUT_x86_64 ?= 15
TEST_TIMEOUT_x86_64_lafs ?= 20
else
TEST_TIMEOUT_x86_64 ?= 60
TEST_TIMEOUT_x86_64_lafs ?= 90
endif
TEST_TIMEOUT_riscv64 := 120

test-x86_64: iso-test
	python3 script/check_runtime_test_modules.py "$(NM)" $(ARCH)
	@echo "=== TEST x86_64 ($(X86_64_QEMU_ACCEL)) ==="
	X86_64_QEMU_ACCEL=$(X86_64_QEMU_ACCEL) \
	X86_64_QEMU_TIMEOUT=$(TEST_TIMEOUT_x86_64) \
	QEMU_SYSTEM_X86_64=$(QEMU_CMD) \
	bash script/test_x86_64.sh $(ISO) $(SERIAL_LOG) \
		$(BUILD_DIR)/test-x86_64-qemu.log \
		$(BUILD_DIR)/test-x86_64-debugcon.log 4

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
test-x86_64-lafs: lafs-image iso-test
	@echo "=== TEST x86_64 LaFS (virtio-blk-pci) ==="
	@rm -f $(SERIAL_LOG)
	timeout $(TEST_TIMEOUT_x86_64_lafs) $(QEMU_CMD) \
		$(QEMU_ACCEL_ARGS) -machine $(QEMU_MACHINE) -cdrom $(ISO) \
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

	python3 script/check_boot_phases.py $(SERIAL_LOG)

# ── x86_64 SMP TLB shootdown remap stress ──
test-x86_64-smp-tlb:
	@$(MAKE) iso X86_64_BOOT_MODE=test X86_64_TEST_CONF=conf/test-x86_64-tlb.conf
	@echo "=== TEST x86_64 SMP TLB shootdown (smp=$(X86_64_QEMU_SMP)) ==="
	@rm -f $(SERIAL_LOG)
	timeout $(TEST_TIMEOUT_x86_64) $(QEMU_CMD) \
		$(QEMU_ACCEL_ARGS) -machine $(QEMU_MACHINE) -cdrom $(ISO) \
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

# ET_EXEC instance arguments: parse private argv into local config.
.PHONY: test-x86_64-task-params
test-x86_64-task-params:
	@$(MAKE) iso X86_64_BOOT_MODE=test X86_64_TEST_CONF=conf/test-x86_64-params.conf
	@QEMU_SYSTEM_X86_64="$(QEMU_CMD)" \
		X86_64_QEMU_ACCEL="$(X86_64_QEMU_ACCEL)" \
		X86_64_QEMU_TIMEOUT="$(TEST_TIMEOUT_x86_64)" \
		bash script/test_x86_64_task_params.sh

test-x86_64-rollback:
	@$(MAKE) iso X86_64_BOOT_MODE=test X86_64_TEST_CONF=conf/test-x86_64-rollback.conf
	@echo "=== TEST x86_64 module rollback ==="
	@rm -f $(SERIAL_LOG)
	timeout $(TEST_TIMEOUT_x86_64) $(QEMU_CMD) \
		$(QEMU_ACCEL_ARGS) -machine $(QEMU_MACHINE) -cdrom $(ISO) \
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
	@if grep -aFq '[load] laos_module_entry or _start not found in module' $(SERIAL_LOG); then \
		echo "  PASS: x86_64 rollback (legacy main entry rejected)"; \
	else \
		echo "  FAIL: x86_64 rollback (legacy main entry was accepted; log: $(SERIAL_LOG))"; exit 1; \
	fi
	if grep -qE "\\[selftest\\] .registry.* PASSED" $(SERIAL_LOG); then \
		echo "  PASS: x86_64 rollback (registry)"; \
	else \
		echo "  FAIL: x86_64 rollback registry (serial log: $(SERIAL_LOG))"; exit 1; \
	fi
	@for marker in '[module-foo] resumed after timeout' '[user-exit] user0 code=0'; do \
		grep -aFq "$$marker" $(SERIAL_LOG) || { echo "  FAIL: x86_64 rollback continuation ($$marker; log: $(SERIAL_LOG))"; exit 1; }; \
	done
	@if grep -aEq '\[PANIC ' $(SERIAL_LOG) || grep -aFq 'KERNEL EXCEPTION' $(SERIAL_LOG); then \
		echo "  FAIL: x86_64 rollback reached an unexpected kernel failure (log: $(SERIAL_LOG))"; exit 1; \
	fi

test-x86_64-negative:
	@$(MAKE) iso X86_64_BOOT_MODE=test X86_64_TEST_CONF=conf/test-x86_64-negative.conf INCLUDE_BAD_MODULE=1
	@echo "=== TEST x86_64 module negative ==="
	@rm -f $(SERIAL_LOG)
	timeout $(TEST_TIMEOUT_x86_64) $(QEMU_CMD) \
		$(QEMU_ACCEL_ARGS) -machine $(QEMU_MACHINE) -cdrom $(ISO) \
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
	@for marker in '[module-foo] resumed after timeout' '[user-exit] user0 code=0'; do \
		grep -aFq "$$marker" $(SERIAL_LOG) || { echo "  FAIL: x86_64 negative continuation ($$marker; log: $(SERIAL_LOG))"; exit 1; }; \
	done
	@if grep -aEq '\[PANIC ' $(SERIAL_LOG) || grep -aFq 'KERNEL EXCEPTION' $(SERIAL_LOG); then \
		echo "  FAIL: x86_64 negative test reached an unexpected kernel failure (log: $(SERIAL_LOG))"; exit 1; \
	fi

test-x86_64-sched-stress:
	@$(MAKE) iso X86_64_BOOT_MODE=test X86_64_TEST_CONF=conf/test-x86_64-stress.conf
	@echo "=== TEST x86_64 scheduler stress (smp=$(X86_64_QEMU_SMP)) ==="
	@rm -f $(SERIAL_LOG)
	timeout $(TEST_TIMEOUT_x86_64) $(QEMU_CMD) \
		$(QEMU_ACCEL_ARGS) -machine $(QEMU_MACHINE) -cdrom $(ISO) \
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
	@$(MAKE) iso X86_64_BOOT_MODE=test X86_64_TEST_CONF=conf/test-x86_64-rcu-stress.conf
	@QEMU_SYSTEM_X86_64="$(QEMU_CMD)" \
		QEMU_MACHINE="$(QEMU_MACHINE)" \
		X86_64_QEMU_SMP="$(X86_64_QEMU_SMP)" \
		X86_64_RCU_STRESS_TIMEOUT="$(TEST_TIMEOUT_x86_64_RCU_STRESS)" \
		bash script/test_x86_64_rcu_stress.sh

test-x86_64-multiuser:
	@$(MAKE) iso X86_64_BOOT_MODE=test X86_64_TEST_CONF=conf/test-x86_64-multiuser.conf
	@echo "=== TEST x86_64 multi-user ==="
	@rm -f $(SERIAL_LOG)
	timeout $(TEST_TIMEOUT_x86_64) $(QEMU_CMD) \
		$(QEMU_ACCEL_ARGS) -machine $(QEMU_MACHINE) -cdrom $(ISO) \
		-m 2G -smp 1 -net none \
		-no-reboot -no-shutdown \
		-display none -serial file:$(SERIAL_LOG) \
		$(QEMU_DEBUG) 2>$(BUILD_DIR)/test-x86_64-multiuser-qemu.log; \
	status=$$?; test $$status -eq 0 -o $$status -eq 124
	@count=$$(grep -acF 'M3c: ELF -> EL0 -> syscall write -> exit, DONE' $(SERIAL_LOG) || true); \
	if [ "$$count" -ge 4 ]; then \
		echo "  PASS: x86_64 multiuser ($$count exits)"; \
	else \
		echo "  FAIL: x86_64 multiuser ($$count exits, expected >= 4, including base init; log: $(SERIAL_LOG))"; exit 1; \
	fi

	@for name in user0 user1 user2; do \
		grep -aFq "[user-exit] $$name code=0" $(SERIAL_LOG) || { echo "FAIL: missing test user exit $$name"; exit 1; }; \
	done

.PHONY: test-boot-context
test-boot-context:
	bash script/test_boot_context.sh

test-task-conf-v1:
	bash script/check_task_conf_v1.sh \
		conf/task.conf \
		conf/task-base.conf \
		conf/test.conf \
		conf/task-arm64.conf \
		conf/task-arm64-negative.conf \
		conf/task-arm64-rollback.conf \
		conf/task-arm64-tlb.conf \
		conf/task-arm64-fpu.conf \
		conf/test-x86_64-rollback.conf \
		conf/test-x86_64-negative.conf \
		conf/test-x86_64-stress.conf \
		conf/test-x86_64-tlb.conf \
		conf/test-x86_64-rcu-stress.conf \
		conf/test-x86_64-multiuser.conf \
		conf/test-x86_64-params.conf \
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
	@echo "  make test-x86_64-runtime-modules  verify module artifacts and missing-payload behaviour"
	@echo "  make test-x86_64-boot-modes  verify explicit test mode and source isolation"
	@echo "  make iso-test     build base initrd plus explicit test.conf and fixtures"
	@echo "  make iso-base     build x86_64 external-initrd boot ISO"
	@echo "  make test-x86_64-initrd  verify external-initrd tasks and exits"
	@echo "  make test-x86_64-boot-payload  run packaging, boot modes, specialized and failure gates"
	@echo "  make initrd       build validated standalone base CPIO and manifest"
	@echo "  make test-initrd  check packaging and rejection paths"
	@echo "  make kernel       build kernel only"
	@echo "  make user         build user ELFs only"
	@echo "  make module       build kernel modules only"
	@echo "  make iso          build $(ISO)"
	@echo "  make run          build iso + launch qemu (host needs tap0)"
	@echo "  make run HEADLESS=1  same as run, but -display none -serial stdio"
	@echo "  make test-boot-context  host checks for boot files and isolated configs"
	@echo "  make test-task-conf-v1 validate task.conf DSL v1 fixtures"
	@echo ""
	@echo "  make test-x86_64-task-params  ET_EXEC per-instance argv and argument validation"
	@echo "  make test-x86_64   CI smoke: build + QEMU boot/selftest check"
	@echo "  make test-x86_64-lafs  x86_64 LaFS via virtio-blk-pci (disk image + mount + selftests)"
	@echo "  make test-x86_64-smp-tlb  x86_64 SMP TLB shootdown remap stress (X86_64_QEMU_SMP=4 default)"
	@echo "  make test-x86_64-rollback  x86_64 module loading transaction rollback"
	@echo "  make test-x86_64-negative  x86_64 reject bad module + selftest init fail"
	@echo "  make test-x86_64-sched-stress  x86_64 scheduler stress (4 CPUs)"
	@echo "  make test-x86_64-rcu-stress  x86_64 configurable preemptible-RCU stress"
	@echo "  make test-x86_64-multiuser  x86_64 multi-user tasks"
	@echo "  make test-arm64    CI smoke: -kernel direct boot (SMP + e1000 + user ELF)"
	@echo "  make test-arm64-limine               Limine UEFI: module ABI + EL0 task chain"
	@echo "  make test-arm64-limine-negative      Limine UEFI: reject bad module, then continue"
	@echo "  make test-arm64-limine-rollback      Limine UEFI: module init failure rollback"
	@echo "  make test-arm64-limine-smp-park      Limine UEFI: SMP AP parking"
	@echo "  make test-arm64-limine-smp-tlb       Limine UEFI: SMP TLB shootdown"
	@echo "  make test-arm64-limine-fpu           Limine UEFI: FPU/SIMD selftest"
	@echo "  make test-arm64-limine-sched-stress  Limine UEFI: scheduler stress (4 CPUs)"
	@echo "  make test-arm64-limine-multiuser     Limine UEFI: multi-user tasks"
	@echo "  make test-riscv64  CI smoke: cross-build + QEMU boot check (TCG, ~60s)"
	@echo "  make test-all      run all three arch tests in order"
	@echo ""
	@echo "  make clean        wipe build artifacts (build/ + bin-*/ + obj-*/)"
	@echo "  make distclean     clean + .cache + compile_commands.json"
	@echo ""
	@echo "Variables:"
	@echo "  ARCH=$(ARCH)  (supported: x86_64, aarch64, riscv64)"

# Standalone base archive. HOST_CC is independent of the kernel cross compiler.
HOST_CC ?= cc
INITRD_TASK_CONF ?= conf/task-base.conf
INITRD := $(BUILD_DIR)/initrd-$(ARCH).cpio
INITRD_MANIFEST := $(BUILD_DIR)/initrd-$(ARCH).json
.PHONY: initrd embedded-initrd module-base test-initrd
module-base:
ifeq ($(ARCH),aarch64)
	$(MAKE) -C module ARCH=$(ARCH) TOOLCHAIN=$(ARM64_TOOLCHAIN) base
else
	$(MAKE) -C module ARCH=$(ARCH) base
endif

$(BUILD_DIR)/host/check-initrd: tests/host/check_initrd.c tests/host/host_support.c tests/host/host_support.h $(wildcard tests/host/include/*.h kernel/*.h) kernel/boot_files.c kernel/cpio.c kernel/task_conf.c kernel/task_parser.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I tests/host/include -I kernel -I kernel/tests tests/host/check_initrd.c tests/host/host_support.c kernel/boot_files.c kernel/cpio.c kernel/task_conf.c kernel/task_parser.c -o $@

initrd: user module-base $(BUILD_DIR)/host/check-initrd
	python3 script/build_initrd.py --arch $(ARCH) --config $(INITRD_TASK_CONF) --output $(INITRD) --manifest $(INITRD_MANIFEST) --validator $(BUILD_DIR)/host/check-initrd --component /task/user.elf=user/bin-$(ARCH)/user.elf --component /task/module_foo.mo=module/bin-$(ARCH)/module_foo.mo --component /task/e1000.mo=module/bin-$(ARCH)/e1000.mo

test-initrd: initrd
	python3 tests/host/test_initrd.py $(ARCH) $(BUILD_DIR)/host/check-initrd

.PHONY: iso-base iso-test test-x86_64-initrd
iso-test:
	$(MAKE) iso X86_64_BOOT_MODE=test

iso-base:
	$(MAKE) iso ARCH=x86_64 X86_64_BOOT_MODE=base BUILD_DIR=$(BUILD_DIR)/base

test-x86_64-initrd: iso-base
	python3 script/test_x86_64_initrd.py $(BUILD_DIR)/base/LaOS.iso $(BUILD_DIR)/base
	python3 script/test_x86_64_initrd_cases.py $(BUILD_DIR)/base

.PHONY: test-x86_64-boot-modes
test-x86_64-boot-modes: iso-base
	python3 script/test_x86_64_boot_modes.py $(BUILD_DIR)/base
	$(MAKE) iso-test BUILD_DIR=$(BUILD_DIR)/mode-switch
	$(MAKE) iso BUILD_DIR=$(BUILD_DIR)/mode-switch
	python3 script/check_base_iso.py $(BUILD_DIR)/mode-switch

# End-to-end stage 8 gates for packaging, boot purposes and expected failures.
test-x86_64-boot-payload:
	$(MAKE) test-task-conf-v1
	$(MAKE) test-initrd
	$(MAKE) test-x86_64-initrd
	$(MAKE) test-x86_64-boot-modes
	$(MAKE) test-x86_64-task-params
	$(MAKE) test-x86_64-multiuser
	$(MAKE) test-x86_64-negative
	$(MAKE) test-x86_64-rollback

.PHONY: test-x86_64-runtime-modules
test-x86_64-runtime-modules: iso-base
	python3 script/check_runtime_test_modules.py "$(NM)" $(ARCH)
	python3 script/test_x86_64_runtime_modules.py $(BUILD_DIR)/base
