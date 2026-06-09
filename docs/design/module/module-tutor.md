# Kernel module loading tutorial

This tutorial follows a relocatable `.mo` file from the build directory to a
runnable kernel thread.  It also explains the related selftest payload path.

LaOS modules are a teaching mechanism.  They demonstrate ELF sections,
relocations, symbol resolution, architecture constraints and transactional
failure handling.  They do not yet provide a stable or unloadable kernel ABI.

## 1. Build a relocatable module

Run:

```sh
make module
```

The module build rules are in `module/Makefile`.  Most `.mo` files are compiler
objects or the result of `ld -r`, so their ELF type is `ET_REL`.  The build uses
freestanding, non-PIC code and architecture-specific flags:

- x86_64 uses the kernel code model and no red zone;
- ARM64 uses the generic ARMv8-A integer instruction set without FP/SIMD in
  ordinary modules.

`module/module_foo.c` is the smallest normal example.  It defines `main()` and
declares configurable globals with `MODULE_PARAM`.

`module/module_abi.c` is the more useful loader fixture.  It exercises
initialised data, BSS, local addresses, imported functions and architecture
relocation forms.

## 2. Put the module in the boot payload

The top-level image rules copy `.mo` files under `/task/` in the boot image.
A row in `task.conf` selects a module, task name, task type, CPU, magic value
and optional arguments/parameters.

For example:

```text
0 module_foo.mo:foo 1 0xa count=2 tick=10
```

The complete syntax is documented in
[Task configuration DSL](../../task-conf-dsl.md).

At boot, `kernel/task.c` finds the named boot module.  Kernel driver/thread
entries are passed to `kthread_load_elf()` with the relocatable-module flag.
After successful loading, task parameters are applied and the new thread is
queued on its configured CPU.

## 3. Preflight the ELF

The shared loader is in `kernel/elf_loader.c`.

First, `elf_check()` in `kernel/elf.c` checks basic ELF identity:

- ELF64;
- little-endian;
- current ELF version;
- `ET_EXEC` or `ET_REL`;
- x86_64 or AArch64 machine identifier;
- presence of the required program- or section-header table.

For an `ET_REL` module, the loader then preflights relocations in allocated
runtime sections.  It rejects:

- a relocation type the current architecture implementation does not support;
- an undefined symbol not present in the kernel export table.

This happens before module-region allocation.  The negative fixture
`module/module_bad.c` deliberately imports a nonexistent symbol to verify this
property.

Debug-section relocations are ignored because debug data is not part of the
runtime module image.

## 4. Allocate the runtime image and BSS

`kernel/module_alloc.c` implements a dedicated virtual-address region with a
16-byte-aligned bump allocator.  Physical pages are mapped on demand.

The dedicated region matters on x86_64: kernel-code-model calls commonly use
32-bit PC-relative relocations, so module code must remain close enough to the
kernel text.  General heap memory is much farther away.

The loader records a checkpoint before allocation.  It copies the complete ELF
file into the module region, then allocates and zeroes every allocated
`SHT_NOBITS` section separately.  Separate BSS allocation is necessary because
an ELF `NOBITS` section may share a file offset with real file data.

There can currently be at most eight BSS segments in one module.

## 5. Resolve symbols and apply relocations

`EXPORT_SYMBOL(name)` in `kernel/export.h` places a name/address pair in the
kernel's `__ksymtab`.  `ksym_lookup()` in `kernel/ksym.c` linearly searches
that table when the module has an undefined symbol.

For a defined module symbol, the loader computes the runtime address from its
section:

- normal allocated sections use the copied ELF image;
- `SHT_NOBITS` symbols use the separately allocated BSS mapping;
- absolute symbols use their encoded value.

The relocation implementation supports these current runtime forms:

| x86_64 | AArch64 |
| --- | --- |
| `R_X86_64_64` | `R_AARCH64_ABS64` |
| `R_X86_64_32`, `R_X86_64_32S` | `R_AARCH64_ABS32` |
| `R_X86_64_PC32`, `R_X86_64_PLT32` | `R_AARCH64_PREL32` |
| `R_X86_64_PC64` | `R_AARCH64_CALL26`, `R_AARCH64_JUMP26` |
| `R_X86_64_NONE` | `R_AARCH64_CONDBR19` |
|  | `R_AARCH64_ADR_PREL_PG_HI21` |
|  | `R_AARCH64_ADD_ABS_LO12_NC` |
|  | `R_AARCH64_LDST8/32/64_ABS_LO12_NC` |
|  | `R_AARCH64_RELATIVE` |

Range and alignment checks reject branch, PC-relative and scaled-load
encodings that cannot represent their target.

After relocation, `arch_module_sync_icache()` establishes executable-code
visibility.  It is a compiler barrier on coherent x86_64 and a data-cache
clean/instruction-cache invalidate sequence on ARM64.

## 6. Find the entry and publish the module

A normal kernel-thread module must define `main` or `_start`.  The loader finds
that symbol in the module's own `.symtab`.  `module/module_no_entry.c` verifies
the missing-entry failure path.

The loader builds a `module_desc`, then reserves a slot in the registry before
creating the thread.  Registry states are:

```text
FREE -> RESERVED -> COMMITTED
                   \
                    -> FREE on cancel before commit
```

Only committed entries are visible to queries.  A failed thread creation
cancels the reservation and rolls the module allocator back to its checkpoint.
A successful creation commits the entry and associates the runtime image with
the thread.

The registry is fixed at 32 entries and is append-only after commit.  There is
no unload operation.

## 7. Apply module parameters

A declaration such as:

```c
static int count = 1;
MODULE_PARAM(count, INT, "print iterations per loop");
```

creates a `struct laos_param` entry in the `__laos_params` ELF section.
After the thread is created but before it is queued, `module_apply_kv_params()`
matches keys from `task.conf` and writes the corresponding module variable.

Supported declared types are:

- `INT`;
- `STRING`;
- `BOOL`.

This facility is intentionally minimal.  The current integer parser does not
validate signs or non-digits, string assignment has no declared destination
capacity, unknown keys do not fail the load, and there is no schema/version
contract.  Treat parameters as trusted boot configuration.

## 8. Selftest payloads

A selftest `.mo` defines `selftest_init` instead of `main`.  The synchronous
path `selftest_load_payload()` performs the same preflight, allocation,
relocation, BSS and cache steps, reserves a module-registry slot, then calls
`selftest_init()`.

The init function normally calls `selftest_register()`.  If init returns a
negative value, the registry reservation and allocator checkpoint are
cancelled.  `module/test_init_fail.c` verifies this path.

One subtle boundary remains: rollback restores loader-owned memory and registry
state; it cannot generally undo arbitrary side effects already performed by a
failing init function.  A real module lifecycle would need an explicit
prepare/commit/abort contract or an exit callback.

## 9. Run the module tests

Useful x86_64 targets:

```sh
make test-x86_64
make test-x86_64-rollback
make test-x86_64-negative
```

Useful ARM64 targets:

```sh
make test-arm64-limine
make test-arm64-limine-rollback
make test-arm64-limine-negative
```

`script/check_arm64_module_abi.sh` also checks that the ARM64 ABI fixture
actually contains the relocation forms the runtime test claims to exercise.

The positive test proves execution and data/BSS relocation.  The rollback and
negative tests prove missing-entry, unresolved-symbol and failed-init paths.
Together they are stronger evidence than a boot marker alone.

## Exercises

1. Tighten all ELF offset/count/range validation before dereferencing tables.
2. Reject a module whose `e_machine` does not match the running architecture.
3. Make module parameters length-aware and fail on malformed values.
4. Add versioned metadata describing the module's required kernel ABI.
5. Split selftest init into side-effect-free prepare and explicit commit.
6. Design unloading only after defining ownership of threads, callbacks,
   exported symbols and cross-CPU execution.
