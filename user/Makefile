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
  CFLAGS   := -ffreestanding -fno-stack-protector -m64
  LDFLAGS  :=
  OBJS     := $(OBJDIR)/crt0.o $(OBJDIR)/main.o $(OBJDIR)/user.lib.o
endif

# ---- aarch64 --------------------------------------------------------
ifeq ($(ARCH),aarch64)
  TOOLCHAIN := aarch64-linux-gnu
  AS        := $(TOOLCHAIN)-as
  CC        := $(TOOLCHAIN)-gcc
  LD        := $(TOOLCHAIN)-ld
  ASFLAGS   :=
  CFLAGS    := -mcpu=cortex-a57 -march=armv8-a+nofp+nosimd \
               -mgeneral-regs-only -ffreestanding -fno-stack-protector
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
