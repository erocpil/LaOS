# Nuke built-in rules.
.SUFFIXES:

# This is the name that our final executable will have.
# Change as needed.
OUTPUT ?= kernel

# Target architecture to build for. Default to x86_64.
ARCH := x86_64

# Install prefix; /usr/local is a good, standard default pick.
PREFIX := /usr/local

# Check if the architecture is supported.
ifeq ($(filter $(ARCH),aarch64 loongarch64 riscv64 x86_64),)
    $(error Architecture $(ARCH) not supported)
endif

# User controllable toolchain and toolchain prefix.
TOOLCHAIN :=
TOOLCHAIN_PREFIX :=
ifneq ($(TOOLCHAIN),)
    ifeq ($(TOOLCHAIN_PREFIX),)
        TOOLCHAIN_PREFIX := $(TOOLCHAIN)-
    endif
endif

# User controllable C compiler command.
ifneq ($(TOOLCHAIN_PREFIX),)
    CC := $(TOOLCHAIN_PREFIX)gcc
else
    CC := cc
endif

# User controllable linker command.
LD := $(TOOLCHAIN_PREFIX)ld

# Defaults overrides for variables if using "llvm" as toolchain.
ifeq ($(TOOLCHAIN),llvm)
    CC := clang
    LD := ld.lld
endif

# User controllable C flags.
CFLAGS := -g -O2 -pipe

# User controllable C preprocessor flags. We set none by default.
CPPFLAGS :=

ifeq ($(ARCH),x86_64)
    # User controllable nasm flags.
    NASMFLAGS := -g -F dwarf
endif

# User controllable linker flags. We set none by default.
LDFLAGS :=

# Ensure the dependencies have been obtained.
# ifneq ($(filter-out clean distclean,$(or $(MAKECMDGOALS),default)),)
#     ifeq ($(wildcard .deps-obtained),)
#         $(error Please run the ./get-deps script first)
#     endif
# endif

# Check if CC is Clang.
override CC_IS_CLANG := $(shell ! $(CC) --version 2>/dev/null | grep -q '^Target: '; echo $$?)

# Internal C flags that should not be changed by the user.
override CFLAGS += \
    -Wall \
    -Wextra \
    -std=gnu11 \
    -nostdinc \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fno-PIC \
    -ffunction-sections \
    -fdata-sections \
    -fno-omit-frame-pointer \
    -fno-optimize-sibling-calls

# Internal C preprocessor flags that should not be changed by the user.
override CPPFLAGS := \
    -I kernel \
    -I kernel/arch/$(ARCH) \
    -I third_party/limine-c-template/kernel/limine-protocol/include \
    -isystem third_party/limine-c-template/kernel/freestnd-c-hdrs/include \
    $(CPPFLAGS) \
    -MMD \
    -MP

ifeq ($(ARCH),x86_64)
    # Internal nasm flags that should not be changed by the user.
    override NASMFLAGS := \
        $(patsubst -g,-g -F dwarf,$(NASMFLAGS)) \
        -Wall \
        -I kernel/arch/$(ARCH)/
endif

# Architecture specific internal flags.
ifeq ($(ARCH),x86_64)
    ifeq ($(CC_IS_CLANG),1)
        override CC += \
            -target x86_64-unknown-none-elf
    endif
    override CFLAGS += \
        -m64 \
        -march=x86-64 \
        -mabi=sysv \
        -mno-80387 \
        -mno-mmx \
        -mno-sse \
        -mno-sse2 \
        -mno-red-zone \
        -mcmodel=kernel
    override LDFLAGS += \
        -m elf_x86_64
    override NASMFLAGS := \
        -f elf64 \
        $(NASMFLAGS)
endif
ifeq ($(ARCH),aarch64)
    override CFLAGS += \
        -mcpu=generic \
        -march=armv8-a+nofp+nosimd \
        -mgeneral-regs-only
    override LDFLAGS += \
        -m aarch64elf
    override LDFLAGS_POST += \
        $(shell $(CC) -print-libgcc-file-name)
endif
ifeq ($(ARCH),riscv64)
    ifeq ($(CC_IS_CLANG),1)
        override CC += \
            -target riscv64-unknown-none-elf
    endif
    override CFLAGS += \
        -march=rv64imac_zicsr_zifencei \
        -mabi=lp64 \
        -mno-relax
    override LDFLAGS += \
        -m elf64lriscv \
        --no-relax
endif
ifeq ($(ARCH),loongarch64)
    ifeq ($(CC_IS_CLANG),1)
        override CC += \
            -target loongarch64-unknown-none-elf
    endif
    override CFLAGS += \
        -march=loongarch64 \
        -mabi=lp64s \
        -mfpu=none \
        -msimd=none
    override LDFLAGS += \
        -m elf64loongarch
endif

# Internal linker flags that should not be changed by the user.
override LDFLAGS += \
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    --gc-sections \
    -T third_party/linker-scripts/$(if $(filter 1,$(LIMINE_AARCH64)),aarch64-limine,$(ARCH)).lds

# Use "find" to glob all *.c, *.S, and *.asm files in the tree
# (except the src/arch/* directories, as those are gonna be added
# in the next step).
override SRCFILES := $(shell find -L kernel cc-runtime/src -type f -not -path 'kernel/arch/*' 2>/dev/null | LC_ALL=C sort)
# Add architecture specific files, if they exist.
override SRCFILES += $(shell find -L kernel/arch/$(ARCH) -type f 2>/dev/null | LC_ALL=C sort)
# Obtain the object and header dependencies file names.
override CFILES := $(filter %.c,$(SRCFILES))
override ASFILES := $(filter %.S,$(SRCFILES))
ifeq ($(LIMINE_AARCH64),1)
    ifeq ($(ARCH),aarch64)
        override ASFILES := $(filter-out %/startup.S,$(ASFILES))
    endif
endif
ifeq ($(ARCH),x86_64)
override NASMFILES := $(filter %.asm,$(SRCFILES))
endif
override OBJ := $(addprefix obj-$(ARCH)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o))
ifeq ($(ARCH),x86_64)
override OBJ += $(addprefix obj-$(ARCH)/,$(NASMFILES:.asm=.asm.o))
endif
override HEADER_DEPS := $(addprefix obj-$(ARCH)/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))

# Generated C sources must be private to both architecture and output. Keeping
# them under kernel/ made concurrent x86_64/aarch64 builds overwrite the same
# kallsyms source while another compiler was reading it.
override GENERATED_DIR := obj-$(ARCH)/generated/$(OUTPUT)
override KALLSYMS_C := $(GENERATED_DIR)/kallsyms_all.c
override KALLSYMS_OBJ := $(KALLSYMS_C).o
override GIT_VERSION_C := $(GENERATED_DIR)/git_version.c
override GIT_VERSION_OBJ := $(GIT_VERSION_C).o

# Filter out any legacy find-discovered copies, then add one private object.
override OBJ := $(filter-out %/kallsyms_all.c.o,$(OBJ)) $(KALLSYMS_OBJ)
override OBJ := $(filter-out %/git_version.c.o,$(OBJ)) $(GIT_VERSION_OBJ)

# Default target. This must come first, before header dependencies.
.PHONY: all
all: bin-$(ARCH)/$(OUTPUT)

ifeq ($(ARCH),x86_64)
# NASM does not emit dependency files.  These two entry paths consume the
# generated struct/CPU offsets directly, so a TCB layout change must rebuild
# them just as it rebuilds C and preprocessed assembly objects.
obj-x86_64/kernel/arch/x86_64/switch.asm.o \
obj-x86_64/kernel/arch/x86_64/syscall_entry.asm.o: \
	kernel/arch/x86_64/asm_offsets_nasm.inc
endif

# Include header dependencies.
-include $(HEADER_DEPS)

# Generate a stub for the first link; the two-pass rule below overwrites it.
$(KALLSYMS_C):
	mkdir -p "$(dir $@)"
	@echo '/* stub — overwritten by gen_kallsyms.sh during build */' > $@
	@echo '#include <stdint.h>' >> $@
	@echo 'const int KALLSYMS_COUNT = 0;' >> $@
	@echo 'struct { uint64_t addr; const char *name; } kallsyms_all[1];' >> $@

# Generate git_version.c with current commit hash.
.PHONY: FORCE
FORCE:

$(GIT_VERSION_C): FORCE
	mkdir -p "$(dir $@)"
	@echo '/* generated — do not edit */' > $@
	@hash=$$(git rev-parse --short=7 HEAD 2>/dev/null || echo "unknown"); \
	echo "const char git_version[] = \"$$hash\";" >> $@

$(KALLSYMS_OBJ): $(KALLSYMS_C) kernel.mk
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(GIT_VERSION_OBJ): $(GIT_VERSION_C) kernel.mk
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Link rules for the final executable.
# Two-pass build: first link without kallsyms, generate symbol table,
# then link again with full symbol resolution.
bin-$(ARCH)/$(OUTPUT): kernel.mk third_party/linker-scripts/$(ARCH).lds $(OBJ)
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $(OBJ) -o $@ $(LDFLAGS_POST)
	script/gen_kallsyms.sh $@ > $(KALLSYMS_C)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $(KALLSYMS_C) -o $(KALLSYMS_OBJ)
	$(LD) $(LDFLAGS) $(filter-out $(KALLSYMS_OBJ),$(OBJ)) $(KALLSYMS_OBJ) -o $@ $(LDFLAGS_POST)

# Compilation rules for *.c files.
obj-$(ARCH)/%.c.o: %.c kernel.mk
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Compilation rules for *.S files.
obj-$(ARCH)/%.S.o: %.S kernel.mk
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

ifeq ($(ARCH),x86_64)
# Compilation rules for *.asm (nasm) files.
obj-$(ARCH)/%.asm.o: %.asm kernel.mk
	mkdir -p "$(dir $@)"
	nasm $(NASMFLAGS) $< -o $@
endif

# Remove object files and the final executable.
.PHONY: clean
clean:
	rm -rf bin-$(ARCH) obj-$(ARCH)
	rm -f kernel/git_version.c kernel/kallsyms_all.c

# Remove everything built and generated including downloaded dependencies.
.PHONY: distclean
distclean:
	rm -rf bin-* obj-* .deps-obtained .cache compile_commands.json freestnd-c-hdrs cc-runtime limine-protocol

# Install the final built executable to its final on-root location.
.PHONY: install
install: all
	install -d "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"
	install -m 644 bin-$(ARCH)/$(OUTPUT) "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)-$(ARCH)"

# Try to undo whatever the "install" target did.
.PHONY: uninstall
uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)-$(ARCH)"
	-rmdir "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"

# ARM64 uses the e1000 driver built into the kernel.  On x86_64 it remains a
# loadable module and registers its ISR with the IDT at runtime.
ifeq ($(ARCH),aarch64)
override OBJ += obj-$(ARCH)/kernel/e1000.c.o obj-$(ARCH)/kernel/protocol.c.o

.PHONY: e1000_objs
e1000_objs: obj-$(ARCH)/kernel/e1000.c.o obj-$(ARCH)/kernel/protocol.c.o

obj-$(ARCH)/kernel/e1000.c.o: module/e1000.c module/e1000.h module/protocol.h
	mkdir -p obj-$(ARCH)/kernel
	$(CC) $(CFLAGS) $(CPPFLAGS) -I module -c module/e1000.c -o obj-$(ARCH)/kernel/e1000.c.o

obj-$(ARCH)/kernel/protocol.c.o: module/protocol.c module/protocol.h
	mkdir -p obj-$(ARCH)/kernel
	$(CC) $(CFLAGS) $(CPPFLAGS) -I module -c module/protocol.c -o obj-$(ARCH)/kernel/protocol.c.o
endif
