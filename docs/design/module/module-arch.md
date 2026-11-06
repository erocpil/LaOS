# Kernel module architecture

ARM64 implementation files, ABI scripts and runtime targets referenced here
live on `arm64`. For the separate user-ELF contract, see the
[user-mode architecture](../user/user-arch.md).

## Scope and trust model

LaOS can load freestanding ELF64 kernel-thread and selftest modules on x86_64
and ARM64.  The implementation is suitable for learning and controlled boot
fixtures.

Modules execute at kernel privilege and are part of the trusted computing
base.  The loader is not a security sandbox.  Its checks should still reject
malformed inputs safely, but a successfully loaded module has unrestricted
kernel access through its code and exported interfaces.

## Two ELF paths

The shared ELF code serves distinct purposes:

| Path | ELF form | Runtime model |
| --- | --- | --- |
| kernel-thread executable | `ET_EXEC` | map program headers at linked addresses |
| user process | `ET_EXEC` | map program headers into a private user address space |
| kernel module | `ET_REL` | copy sections, allocate BSS, relocate, create kernel thread |
| selftest payload | `ET_REL` | relocate, call `selftest_init` synchronously |

The module contract should not be inferred from the user-process path.  They
share parsing helpers but have different address spaces, entries, lifetimes
and privilege.

## Loading transaction

For an `ET_REL` kernel-thread module:

```text
ELF check
  -> relocation/symbol preflight
  -> disable local interrupts
  -> allocator checkpoint
  -> allocate and copy ELF
  -> allocate/zero BSS
  -> apply relocations
  -> synchronize I-cache
  -> find laos_module_entry/_start
  -> reserve registry slot
  -> create thread
  -> commit registry slot
  -> restore local interrupts
```

Failures after the checkpoint return the module bump allocator to the saved
state.  A reserved registry entry is cancelled if later work fails.  Preflight
rejects deterministic unresolved-symbol and unsupported-relocation errors
before consuming virtual address space.

The transaction assumes module loads are effectively serialized.  The
allocator operations themselves take a lock, but a checkpoint followed by
later rollback is not an independent transaction if another loader can commit
allocations between those calls.  Disabling local interrupts does not by
itself serialize other CPUs.  Concurrent module loading therefore remains
outside the supported contract.

## Memory model

The module allocator:

- owns an architecture-selected virtual region;
- advances a bump pointer in 16-byte units;
- maps 4 KiB physical pages as required;
- can roll back the most recent transaction;
- cannot free a committed module.

The copied ELF file remains the runtime backing for normal sections.  Up to
eight allocated `SHT_NOBITS` sections receive separate zeroed allocations.

`module_free()` is deliberately a no-op.  Replacing it with page freeing would
not create safe unloading: live threads, function pointers, registered
selftests, device callbacks, symbols and executing CPUs must all be accounted
for first.

The current mappings are writable while module code executes.  There is no
final section-permission pass that makes code read/execute and data
read/write/non-execute.  W^X enforcement is therefore not part of the current
module guarantee.

## Symbol boundary

The supported import namespace is the statically linked kernel
`__ksymtab`.  Kernel code opts into that namespace with `EXPORT_SYMBOL`.
Lookup is by exact string and is linear.

Current consequences:

- no symbol versions;
- no namespace ownership;
- no dependency graph;
- no license or capability policy;
- no duplicate-name policy beyond the static table;
- no registration of one loaded module's exports for another module;
- no reference count preventing removal.

The full kallsyms table is used for diagnostics and address-to-name reporting,
not as the module import policy.  Exporting a function is therefore an
intentional ABI decision even though that ABI is not yet versioned.

## Relocation boundary

Relocation support is an allowlist in `kernel/elf_loader.c`, with encoding in
`kernel/ksym.c`.  Only allocated runtime target sections are processed.
Unsupported forms fail loading.

Architecture-specific constraints include:

- x86_64 PC-relative calls require the module region to remain within signed
  32-bit reach of kernel text;
- AArch64 direct branches and conditional branches have their architectural
  ranges;
- AArch64 page/low-12 relocation pairs must preserve instruction encoding and
  alignment;
- relocated ARM64 code requires explicit instruction-cache synchronisation.

`module/tests/test_module_abi.c` is the executable specification for common relocation
and data behaviour.  `script/check_arm64_module_abi.sh` guards against a
compiler change silently removing an intended ARM64 relocation from the
fixture.

## Registry state

`kernel/module.c` owns a 32-entry fixed registry:

| State | Visible | Can transition to |
| --- | --- | --- |
| free | no | reserved |
| reserved | no | committed or free |
| committed | yes | none |

Names are copied into persistent heap storage at reservation.  IDs increase
monotonically.  Committed descriptors and their copied names remain valid for
the rest of boot.

The registry lock protects state transitions and enumeration.  Find-by-ID
returns a pointer after releasing the lock; this is currently stable only
because committed entries are never changed or removed.

## Entry and parameter contracts

A kernel-thread module must provide `laos_module_entry(int argc, char **argv)`.
The loader temporarily retains `_start` as a legacy fallback, but no longer
accepts `main`.  Its thread is created before configuration parameters are
applied, but it is not queued until after parameter application.

Configured kernel entries receive `argc/argv`. Each launch allocates one
private block for the pointer array and copied strings: `argv[0]` is the task
name, followed by positional arguments and then reconstructed `key=value`
tokens, with a terminating NULL pointer. At most 32 arguments follow the name;
excess arguments reject the launch. The parser's existing buffer limits still
apply. Positional/kv interleaving is not preserved.

The entry borrows these strings read-only. On creation failure the launcher
frees the block; on success `thread.entry_args_owned` owns it until
`thread_destroy()`. An entry must copy any data needed beyond its own thread's
lifetime. No pointer refers to the launcher's stack or mutable task parser
scratch storage. `task_create_user()` retains its existing demonstration
argument conversion, and the user loader copies the result into user memory.

`TASK_THREAD` does not inject globals. Its entry parses arguments into local
configuration. `module/module_foo.c` demonstrates the same pattern for a
`TASK_DRIVER`: `count` and `tick` live in a stack-local `foo_config`.
`module/task_args.h` supplies bounded unsigned integer and boolean parsing;
each task owns its defaults, accepted keys, string bounds and error policy.

`MODULE_PARAM` remains available for module-wide `TASK_DRIVER` configuration,
such as e1000 settings; it is not per-thread configuration. Only the driver
adapter automatically applies it before enqueueing. User and TASK_THREAD
creation do not call the injector.

`make test-x86_64-task-params` starts two threads from the same ET_EXEC image
with different integer, boolean and string arguments. Both must retain their
own values after timer handoffs; a third instance must reject an invalid
integer. The fixture has no mutable global configuration. Sharing immutable
code does not isolate writable image globals: this contract does not make
arbitrary fixed-address ET_EXEC images safe to reload concurrently.
`elf_load_segment()` masks page attributes from reused physical translations
so a second load of the fixture's read-only pages does not treat NX as an
address bit. The gate covers x86_64 ET_EXEC, not an ARM64 executable port.

The parameter metadata stores live pointers into relocated module memory.
The loader writes values directly according to the declared type.  There is no
destination-size field for strings, validation callback, required-key marker
or immutable-after-start rule.

For the present trusted configuration, module authors must ensure:

- integer and boolean values are in the accepted simple syntax;
- a `STRING` destination is large enough;
- parameter storage remains valid for the module's lifetime;
- parameters are not concurrently accessed before the task is queued.

## Selftest init contract

A selftest payload must export a module-local `selftest_init` symbol that
returns zero or positive on success and negative on failure.  Registry
reservation occurs before the call and commit after success.

Loader rollback covers:

- the reserved registry entry and copied name;
- module-region allocations since the checkpoint.

It does not automatically reverse:

- selftests already registered by the init function;
- threads or allocations created by init;
- callbacks installed in other subsystems;
- writes to global kernel state.

Selftest init functions that can fail should therefore avoid externally
visible side effects before their final success decision.

## Validation matrix

| Concern | Fixture/target |
| --- | --- |
| normal load and execution | `module/module_foo.c`, architecture boot tests |
| data, BSS and relocation ABI | `module/tests/test_module_abi.c` |
| ARM64 fixture relocation presence | `script/check_arm64_module_abi.sh` |
| unresolved symbol before allocation | `module/tests/test_module_bad.c`, negative targets |
| legacy `main` rejection and allocator rollback | `module/tests/test_module_no_entry.c`, rollback targets |
| failed selftest init and registry cancel | `module/tests/test_init_fail.c`, negative targets |
| registry remains usable | `registry` selftest marker after failure scenarios |

Primary integration targets are:

- `make test-x86_64`
- `make test-x86_64-rollback`
- `make test-x86_64-negative`
- `make test-arm64-limine`
- `make test-arm64-limine-rollback`
- `make test-arm64-limine-negative`

## Known robustness gaps

The most important gaps are not additional relocation opcodes:

1. ELF table offsets, counts, section links and string offsets need complete
   bounds and overflow validation against the supplied file size.
2. The ELF machine must match the running architecture, not merely be one of
   the two globally recognised values.
3. Allocation checkpoint/rollback needs a global load transaction or another
   concurrency-safe ownership model.
4. Runtime mappings need explicit final permissions and a stated W^X policy.
5. Module parameters need safe parsing and destination bounds.
6. A versioned import ABI is needed before external modules can be expected to
   survive kernel changes.

## Evolution boundary

For a teaching kernel, reliable load, relocation, failure rollback and
cross-architecture fixtures are more valuable than implementing unload.
Unload should come only with a lifecycle design covering dependencies,
quiescence, CPU synchronisation and subsystem registrations.

A practical next milestone is a small versioned module manifest containing
architecture, ABI version, required symbols and parameter metadata.  It would
make the current implicit contract inspectable without committing the project
to Linux-compatible module semantics.
