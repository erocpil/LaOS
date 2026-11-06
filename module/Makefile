# 模块构建 —— 支持 x86_64 和 aarch64
#
# 用法：
#   make ARCH=x86_64                  (默认)
#   make ARCH=aarch64 TOOLCHAIN=aarch64-linux-gnu

ARCH ?= x86_64
TOOLCHAIN ?=

# 工具链
TOOLCHAIN_PREFIX :=
ifneq ($(TOOLCHAIN),)
    TOOLCHAIN_PREFIX := $(TOOLCHAIN)-
endif
ifneq ($(TOOLCHAIN_PREFIX),)
    CC := $(TOOLCHAIN_PREFIX)gcc
    LD := $(TOOLCHAIN_PREFIX)ld
else
    CC := cc
    LD := ld
endif
ifeq ($(ARCH),aarch64)
    LLVM_TARGET ?= aarch64-none-elf
    ifeq ($(TOOLCHAIN),llvm)
        CC := clang
        LD := ld.lld
        LLVM_FLAGS := --target=$(LLVM_TARGET)
    endif
endif

OBJDIR := obj-$(ARCH)
BINDIR := bin-$(ARCH)

# 共享 CFLAGS（对齐 kernel.mk）
SHARED_CFLAGS := \
    -ffreestanding -nostdlib -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -fno-pie -fno-PIC -O2 -g \
    -Wall -Wextra -std=gnu11 \
    -I ../kernel -I ../kernel/arch/$(ARCH) \
    -I ../third_party/limine-c-template/kernel/limine-protocol/include \
    -isystem ../third_party/limine-c-template/kernel/freestnd-c-hdrs/include

# 架构特定 CFLAGS
ifeq ($(ARCH),x86_64)
    # Match kernel.mk's ISA baseline even when the compiler defaults to x86-64-v3.
    ARCH_CFLAGS := -m64 -march=x86-64 -mno-red-zone -mcmodel=kernel -mno-sse -mno-mmx
    LDFLAGS     := -m elf_x86_64
endif
ifeq ($(ARCH),aarch64)
    ARCH_CFLAGS := -mcpu=generic -march=armv8-a+nofp+nosimd -mgeneral-regs-only
    LDFLAGS     :=
endif

CFLAGS := $(SHARED_CFLAGS) $(ARCH_CFLAGS) $(LLVM_FLAGS)

.PHONY: all clean base
.DEFAULT_GOAL := all
base: $(BINDIR)/e1000.mo $(BINDIR)/module_foo.mo
all: $(BINDIR)/e1000.mo
all: $(BINDIR)/module_foo.mo
all: $(BINDIR)/module_abi.mo
all: $(BINDIR)/test_tlb.mo
all: $(BINDIR)/test_cpu_alive.mo
all: $(BINDIR)/test_ipi_delivery.mo
ifeq ($(ARCH),aarch64)
all: $(BINDIR)/module_smp_probe.mo
all: $(BINDIR)/test_init_fail.mo
all: $(BINDIR)/test_fpu_arm64.mo
endif
ifeq ($(ARCH),x86_64)
all: $(BINDIR)/thread_bar.elf
all: $(BINDIR)/test_task_params.elf
all: $(BINDIR)/test_fpu_context.mo
endif
all: $(BINDIR)/module_bad.mo
all: $(BINDIR)/module_no_entry.mo
all: $(BINDIR)/test_init_fail.mo
all: $(BINDIR)/test_sched_stress.mo

# --- 1. thread_bar (x86_64 独立 ELF) ---
ifeq ($(ARCH),x86_64)
$(OBJDIR)/thread_bar.o: thread_bar.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

$(BINDIR)/thread_bar.elf: $(OBJDIR)/thread_bar.o link.ld Makefile
	mkdir -p "$(dir $@)"
	$(LD) -n -T link.ld -o $@ $< -static

$(OBJDIR)/tests/test_task_params.o: tests/test_task_params.c task_args.h Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

$(BINDIR)/test_task_params.elf: $(OBJDIR)/tests/test_task_params.o link.ld Makefile
	mkdir -p "$(dir $@)"
	$(LD) -n -T link.ld -o $@ $< -static

endif

# --- 2. module_foo (双架构 smoke module) ---
$(BINDIR)/module_foo.mo: module_foo.c task_args.h Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# --- 3. module_abi (loader relocation/data regression module) ---
$(BINDIR)/module_abi.mo: tests/test_module_abi.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# Shared dependencies for all selftest modules.
SELFTEST_DEPS := Makefile ../kernel/tests/selftest.h
ifeq ($(ARCH),aarch64)
$(BINDIR)/module_smp_probe.mo: tests/test_module_smp_probe.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)
endif

$(BINDIR)/module_bad.mo: tests/test_module_bad.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

$(BINDIR)/module_no_entry.mo: tests/test_module_no_entry.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

$(BINDIR)/test_init_fail.mo: tests/test_init_fail.c $(SELFTEST_DEPS)
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# --- 5. test_tlb (dual-arch TLB shootdown selftest) ---
$(BINDIR)/test_tlb.mo: tests/test_tlb.c $(SELFTEST_DEPS)
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# --- 6. test_cpu_alive (dual-arch CPU alive selftest) ---
$(BINDIR)/test_cpu_alive.mo: tests/test_cpu_alive.c $(SELFTEST_DEPS)
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# --- 4. e1000 (多文件模块，双架构) ---
$(OBJDIR)/e1000.o: e1000.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

$(OBJDIR)/protocol.o: protocol.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

$(BINDIR)/e1000.mo: $(OBJDIR)/e1000.o $(OBJDIR)/protocol.o Makefile
	mkdir -p "$(dir $@)"
	$(LD) -r $(OBJDIR)/e1000.o $(OBJDIR)/protocol.o -o $@ $(LDFLAGS)

clean:
	rm -rf $(OBJDIR) $(BINDIR)

# --- 7. test_ipi_delivery (dual-arch IPI stress selftest) ---
$(BINDIR)/test_ipi_delivery.mo: tests/test_ipi_delivery.c $(SELFTEST_DEPS)
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# --- 8. test_fpu_context (x86_64 FPU context save/restore selftest) ---
ifeq ($(ARCH),x86_64)
$(BINDIR)/test_fpu_context.mo: tests/test_fpu_context.c $(SELFTEST_DEPS)
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)
endif
# --- stress test (dual-arch scheduler stress selftest) ---
$(BINDIR)/test_sched_stress.mo: tests/test_sched_stress.c $(SELFTEST_DEPS)
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# --- 9. test_fpu_arm64 (ARM64 NEON context save/restore selftest) ---
ifeq ($(ARCH),aarch64)
$(OBJDIR)/test_fpu_arm64.o: tests/test_fpu_arm64.c $(SELFTEST_DEPS)
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

$(OBJDIR)/test_fpu_arm64_helpers.o: tests/test_fpu_arm64_helpers.S Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

$(BINDIR)/test_fpu_arm64.mo: $(OBJDIR)/test_fpu_arm64.o $(OBJDIR)/test_fpu_arm64_helpers.o Makefile
	mkdir -p "$(dir $@)"
	$(LD) -r $(OBJDIR)/test_fpu_arm64.o $(OBJDIR)/test_fpu_arm64_helpers.o -o $@ $(LDFLAGS)

endif

# Runtime tests are delivered only by explicit test/compatibility boot.
RUNTIME_TESTS := priority remote_enqueue rcu_publish rcu_stress
all: $(addprefix $(BINDIR)/test_,$(addsuffix .mo,$(RUNTIME_TESTS)))
$(addprefix $(BINDIR)/test_,$(addsuffix .mo,$(RUNTIME_TESTS))): $(BINDIR)/test_%.mo: tests/test_%.c $(SELFTEST_DEPS) ../kernel/rcu.h ../kernel/mutex.h
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)
