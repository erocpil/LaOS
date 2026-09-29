# x86_64 module build.
#
# 用法：
#   make ARCH=x86_64                  (默认)

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
    ARCH_CFLAGS := -m64 -mno-red-zone -mcmodel=kernel -mno-sse -mno-mmx
    LDFLAGS     := -m elf_x86_64
endif

CFLAGS := $(SHARED_CFLAGS) $(ARCH_CFLAGS)

.PHONY: all clean
all: $(BINDIR)/e1000.mo
all: $(BINDIR)/module_foo.mo
all: $(BINDIR)/module_abi.mo
all: $(BINDIR)/test_tlb.mo
all: $(BINDIR)/test_cpu_alive.mo
all: $(BINDIR)/test_ipi_delivery.mo
ifeq ($(ARCH),x86_64)
all: $(BINDIR)/thread_bar.elf
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
endif

# --- 2. module_foo ---
$(BINDIR)/module_foo.mo: module_foo.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# --- 3. module_abi (loader relocation/data regression module) ---
$(BINDIR)/module_abi.mo: module_abi.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# Shared dependencies for all selftest modules.
SELFTEST_DEPS := Makefile ../kernel/selftest.h

$(BINDIR)/module_bad.mo: module_bad.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

$(BINDIR)/module_no_entry.mo: module_no_entry.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

$(BINDIR)/test_init_fail.mo: test_init_fail.c $(SELFTEST_DEPS)
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# --- 5. test_tlb (TLB shootdown selftest) ---
$(BINDIR)/test_tlb.mo: test_tlb.c $(SELFTEST_DEPS)
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# --- 6. test_cpu_alive selftest ---
$(BINDIR)/test_cpu_alive.mo: test_cpu_alive.c $(SELFTEST_DEPS)
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# --- 4. e1000 multi-file module ---
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
$(BINDIR)/test_ipi_delivery.mo: test_ipi_delivery.c $(SELFTEST_DEPS)
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# --- 8. test_fpu_context (x86_64 FPU context save/restore selftest) ---
ifeq ($(ARCH),x86_64)
$(BINDIR)/test_fpu_context.mo: test_fpu_context.c $(SELFTEST_DEPS)
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)
endif
# --- stress test ---
$(BINDIR)/test_sched_stress.mo: test_sched_stress.c $(SELFTEST_DEPS)
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)
