# 目标架构。可通过 make ARCH=aarch64 覆盖，默认 x86_64。
ARCH ?= x86_64

OBJDIR := obj-$(ARCH)
BINDIR := bin-$(ARCH)

# ---- x86_64 ---------------------------------------------------------
ifeq ($(ARCH),x86_64)
  AS       := nasm
  CC       := gcc
  LD       := ld
  ASFLAGS  := -f elf64
  TARGET_FLAGS :=
  # On Apple Silicon, gcc is Apple Clang targeting the host (arm64) by
  # default.  Compile this x86_64-only source with an explicit freestanding
  # target so constraints such as "=a" remain valid.
  ifneq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
    CC := clang
    LD := ld.lld
    TARGET_FLAGS := --target=x86_64-unknown-none-elf
  endif
  CFLAGS   := -ffreestanding -fno-stack-protector -m64 -march=x86-64 $(TARGET_FLAGS)
  LDFLAGS  := -m elf_x86_64
  OBJS     := $(OBJDIR)/crt0.o $(OBJDIR)/main.o $(OBJDIR)/user.lib.o
endif

# ---- aarch64 --------------------------------------------------------
ifeq ($(ARCH),aarch64)
  TOOLCHAIN ?= aarch64-linux-gnu
  LLVM_TARGET ?= aarch64-none-elf
  ifeq ($(TOOLCHAIN),llvm)
    AS        := clang
    CC        := clang
    LD        := ld.lld
    ASFLAGS   := --target=$(LLVM_TARGET) -c
    TARGET_FLAGS := --target=$(LLVM_TARGET)
  else
    AS        := $(TOOLCHAIN)-as
    CC        := $(TOOLCHAIN)-gcc
    LD        := $(TOOLCHAIN)-ld
    ASFLAGS   :=
    TARGET_FLAGS :=
  endif
  CFLAGS    := -mcpu=cortex-a57 -march=armv8-a+nofp+nosimd \
               -mgeneral-regs-only -ffreestanding -fno-stack-protector \
               $(TARGET_FLAGS)
  LDFLAGS   := -m aarch64elf
  OBJS      := $(OBJDIR)/crt0_arm64.o $(OBJDIR)/main.o $(OBJDIR)/user.lib_arm64.o
endif

ELF := $(BINDIR)/user.elf

.PHONY: all clean
all: $(ELF)

# 汇编入口 — 用 .s 后缀统一
$(OBJDIR)/%.o: %.s Makefile
	mkdir -p "$(dir $@)"
	$(AS) $(ASFLAGS) $< -o $@

# C 源文件
$(OBJDIR)/%.o: %.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) -c $< -o $@ $(CFLAGS)

# 链接
$(ELF): $(OBJS) linker.ld Makefile
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) -T linker.ld $(OBJS) -o $@

clean:
	rm -rf obj-x86_64 obj-aarch64 bin-x86_64 bin-aarch64
