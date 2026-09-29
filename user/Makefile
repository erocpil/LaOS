# x86_64 user-space runtime.
ARCH ?= x86_64

OBJDIR := obj-$(ARCH)
BINDIR := bin-$(ARCH)

AS       := nasm
CC       := gcc
LD       := ld
ASFLAGS  := -f elf64
CFLAGS   := -ffreestanding -fno-stack-protector -m64
LDFLAGS  :=
OBJS     := $(OBJDIR)/crt0.o $(OBJDIR)/main.o $(OBJDIR)/user.lib.o

ELF := $(BINDIR)/user.elf

.PHONY: all clean
all: $(ELF)

# Assembly entry point.
$(OBJDIR)/%.o: %.s Makefile
	mkdir -p "$(dir $@)"
	$(AS) $(ASFLAGS) $< -o $@

# C source files.
$(OBJDIR)/%.o: %.c Makefile
	$(CC) -c $< -o $@ $(CFLAGS)

# Link.
$(ELF): $(OBJS) linker.ld Makefile
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) -T linker.ld $(OBJS) -o $@

clean:
	rm -rf obj-x86_64 bin-x86_64
