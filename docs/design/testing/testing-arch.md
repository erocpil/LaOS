# Testing architecture and coverage matrix

## Goals

LaOS testing is designed to provide:

- fast feedback for documents and configuration fixtures;
- deterministic checks for small kernel components;
- runtime evidence for architecture, privilege and device paths;
- focused negative tests for important failure contracts;
- serial logs that make failures diagnosable without a debugger.

The suite is not a formal verification system, a hardware qualification suite
or a security fuzzer.  Most runtime coverage uses QEMU and trusted boot
fixtures.

## Test taxonomy

| Class | Runs where | Strength | Main limitation |
| --- | --- | --- | --- |
| documentation/static checker | host shell | fast structural consistency | no compiled/runtime semantics |
| fixture grammar checker | host shell | catches invalid checked-in DSL shape | not the kernel parser implementation |
| early synchronous kernel test | boot CPU before scheduling | focused deterministic logic | failure stops boot before device registration |
| registered selftest | kernel after configuration/SMP setup | stateful SMP and subsystem behaviour | serialized, timer-driven, QEMU timing |
| QEMU integration target | host plus full kernel | boot/device/ABI/privilege evidence | emulated hardware and marker-based oracle |
| negative integration target | host plus full kernel | proves selected failures are rejected | limited malformed/fault space |

## Ownership and data flow

`kernel/task_parser.c` parses `@test` text into
`struct selftest_directive`.  `kernel/task_conf.c` owns the directive list.
The parser does not call test implementations. Each explicit `task_conf` context
owns its task/directive lists and policy. On x86_64, normal boot uses the base
initrd context. Explicit test mode adds a separate Limine test context and passes
only that context to the single-session selftest engine. Per-CPU base and fixture
submission are independent. Compatibility mode remains explicit; it is not a
shared parser cursor. See the [boot-file/context contract](../boot/boot-arch.md#boot-file-sets-and-configuration-contexts).

`kernel/tests/selftest.c` owns:

- the registered-test list;
- the active configured-test list;
- optional payload loading;
- configuration and prepare calls;
- serial start/tick/completion;
- canonical result logging.

Individual built-in or module test files own their configuration schema,
workers, completion and pass criteria.

The top-level `Makefile` owns build/run compositions and serial-marker gates.
Scripts under `script/` own more involved QEMU lifecycles and host-side fixture
checks.  `.github/workflows/build.yml` chooses the hosted default subset.

## Startup dependencies and completion conditions

`kernel/boot_phase.c` records ordered BSP completion points. Each transition
requires the preceding phase and disabled local IRQs; duplicate or skipped
transitions panic. The boot functions retain their architecture-specific work.
The phase tracker records a completed contract, not a replacement for device
initialisation, error handling or the architecture's AP rendezvous.

| Completion point | Required work before publication |
| --- | --- |
| `memory` | usable CPU context, physical/virtual memory, heap, exception and interrupt-controller setup; IRQs remain masked |
| `bootstrap-tests` | synchronous VMA/CPIO/LaFS/block-device tests passed; temporary parser, mount and device-registry state cleared |
| `devices` | platform device probing/registration and storage checks attempted, or explicitly skipped by that boot contract |
| `runtime` | boot/idle thread contexts, run queues and configured-task state ready for handoff |
| `smp` | all expected APs reached the architecture's online rendezvous |
| `services` | configured selftests started and BSP tasks queued; scheduling/IRQ handoff may follow |

An optional device being absent does not invalidate the `devices` completion
point. It does not prove that a filesystem mounted or that a device works;
those claims still require the integration target's device-specific markers.
An AP reports online before waiting for `services`. The BSP must not wait for
an AP to execute configured tasks while that AP waits for the BSP. Phase
publication uses a release store and AP observation uses acquire loads.

`kernel_tests_run_bootstrap()` requires `memory`, disabled IRQs and an empty
block-device registry. Its fixtures temporarily replace shared CPIO and LaFS
state and install mock devices. The runner clears those globals before
publishing `bootstrap-tests`; failure or incomplete cleanup stops boot.
`block_device_reset()` is restricted to this early phase so a later test
cannot silently unregister live devices.

The `vma`, `cpio`, `lafs` and `block_device` registered selftests have no
`start` callback. Their `done`/`passed` callbacks report the cached bootstrap
result. Selecting them with `@test` therefore cannot rerun destructive fixtures
from a timer callback after real devices or the real initrd have been loaded.

`script/check_boot_phases.py` requires all six completion markers in order
and one early PASS marker per fixture. Its `--expect-bad-lafs` mode checks
rejection between `bootstrap-tests` and `devices`, excluding the earlier
memory-backed unit-test mounts. Normal boot and storage gates retain their
own device, user-mode and runtime-selftest assertions.

## Selftest lifecycle invariants

```text
REGISTERED
    |
    | matching directive
    v
CONFIGURED -- prepare failure --> terminal FAILED
    |
    v
PREPARED
    |
    | serial scheduler selects it
    v
STARTED -- tick/done --> PASSED or FAILED
```

Required invariants:

- registration occurs before directives are applied;
- configuration and prepare occur before start;
- only one active test is started at a time;
- a test is removed and its instance freed after terminal reporting;
- the next test starts only after the previous terminal result;
- worker allocation happens outside interrupt context;
- a test that can fail must expose an explicit pass predicate;
- asynchronous tests must eventually become done, normally through a timeout.

The framework is lazily initialised because timer activity may call
`selftest_tick()` before any test is registered.

## Configuration limits

An `@test` directive stores:

- a test name of at most 31 characters plus terminator;
- at most eight key/value pairs;
- keys and values of at most 31 characters plus terminator;
- an optional module registry ID filled after payload loading.

Long keys and values are truncated by the kernel directive parser.  The host
fixture checker enforces syntax but does not model every kernel truncation or
allocation behaviour.  Tests should reject missing and out-of-range values in
their own `configure` or `prepare` logic.

Duplicate directives and duplicate test registrations do not have a fully
specified policy.  The current apply loop activates the first matching
registered instance for each directive.  New tests should use unique names.

## Result oracle

For registered selftests, the canonical terminal log is:

```text
[selftest] 'test-name' PASSED
[selftest] 'test-name' FAILED
```

Integration scripts additionally use subsystem-specific markers.  A robust
oracle combines:

- positive evidence for the intended operation;
- absence of panic;
- negative evidence where a forbidden success is meaningful;
- exact counts for SMP/multi-task behaviour when possible.

Serial text is an interface between the kernel and test harness.  Changing a
gated log line is therefore a test-contract change and must update the
corresponding Make rule/script in the same commit.

## Coverage matrix

The following matrix describes maintained targets, not a claim of exhaustive
path coverage.

| Target | Architecture/path | Primary evidence |
| --- | --- | --- |
| `bash script/check_doc_links.sh` | host | Markdown links, anchors, source paths, Make targets |
| `make test-task-conf-v1` | host | checked-in DSL v1 fixture syntax |
| `make test-x86_64` | x86_64 ISO, 4 CPU | boot; priority/PI, registry, remote enqueue, RCU publication, CPU, IPI, TLB and FPU selftests |
| `make test-x86_64-lafs` | x86_64 PCI virtio | real virtio-blk mount plus normal selftests |
| `make test-x86_64-smp-tlb` | x86_64 SMP | IPI delivery and repeated TLB-remap visibility |
| `make test-x86_64-rollback` | x86_64 module loader | missing-entry allocation rollback and registry health |
| `make test-x86_64-negative` | x86_64 module loader | unresolved import rejection and failed init cancel |
| `make test-x86_64-sched-stress` | x86_64 SMP | scheduler stress with worker tasks |
| `make test-x86_64-rcu-stress` | x86_64 SMP | configurable preempted-reader, grace-period and RCU-list stress |
| `make test-x86_64-multiuser` | x86_64 ring 3 | at least three user completion messages before exit |
| `make test-arm64` | ARM64 direct boot | EL0/SVC chain and e1000 discovery |
| `make test-arm64-limine` | ARM64 UEFI/Limine, 2 CPU | module ABI, parameters, EL0, priority/PI, remote wakeup and RCU publication |
| `make test-arm64-limine-negative` | ARM64 UEFI/Limine | unresolved import and failed selftest init |
| `make test-arm64-limine-rollback` | ARM64 UEFI/Limine | missing entry and module allocation rollback |
| `make test-arm64-limine-smp-park` | ARM64 SMP | AP parking/GIC/online/task CPU markers |
| `make test-arm64-limine-smp-tlb` | ARM64 SMP | SGI acknowledgements and repeated TLB remap |
| `make test-arm64-limine-fpu` | ARM64 | FP/SIMD context selftest |
| `make test-arm64-limine-sched-stress` | ARM64 SMP | scheduler stress |
| `make test-arm64-limine-multiuser` | ARM64 EL0 | multiple user-task completion |
| `make test-arm64-lafs` | ARM64 direct boot/MMIO | LaFS mount and known file contents |
| `make test-arm64-lafs-negative` | ARM64 direct boot/MMIO | bad magic reported and mount forbidden |
| `make test-riscv64` | conditional RISC-V path | boot marker when architecture directory exists |

`make test-riscv64` skips successfully when the RISC-V architecture directory
does not exist. ARM64 targets are absent from the x86_64 snapshot's Makefile
and must be run on `arm64`; older branch snapshots may instead contain skip
recipes. A skipped target is not runtime evidence for that architecture.

The x86_64 LaFS target asserts the real mount marker, not the contents of
every printed file. The multi-user target counts a historical marker that
says `EL0`, but x86_64 uses ring 3. That message precedes the exit syscall and
does not establish completed teardown or leak-free reclamation. See the
[user-mode architecture](../user/user-arch.md#process-model-and-validation-limits).

The SMP-park target uses the default task file unless
`ARM64_TASK_CONF=conf/task-arm64.conf` is supplied. Set
`ARM64_EXPECT_SMP_TASK_CPUS=1` (or a comma-separated CPU list) when explicitly
testing the one-shot `module_smp_probe.mo`; the normal default gate deliberately
does not schedule probe modules.

## Hosted CI matrix

The `main` workflow in `.github/workflows/build.yml` runs the following gates.
This describes configured coverage; consult the Actions run for an actual result.

| CI job/step | Coverage |
| --- | --- |
| documentation checker | Markdown references and host ASan/UBSan boot-context checks |
| `make test-x86_64-boot-payload` | config syntax, packaging, external initrd, base/test/compat modes, arguments, multiuser, module rejection and rollback |
| `make test-x86_64-runtime-modules` | ELF symbol ownership and no missing-payload fallback |
| `make test-x86_64-rcu-stress` | focused RCU pressure |
| `make test-x86_64` | four-CPU runtime selftests, including scheduler, TLB and FPU |
| `make test-x86_64-lafs` | real virtio-blk/LaFS integration |

Negative/rollback gates require worker progress and a named user exit after the
expected failure and reject unexpected kernel panic/exception diagnostics.
Boot-payload artifacts include logs under `build/base/`, `build/mode-switch/`,
`build/serial.log` and focused QEMU/argument logs. The later smoke step has its own failure-log upload; the LaFS recipe writes
its diagnostic files under `build/`. ARM64 runtime gates belong to the `arm64` branch;
this workflow does not validate that architecture. Longer focused TLB/scheduler
runs, ARM64 direct boot and hardware coverage remain separate evidence.

## Change-to-test policy

Use the narrowest test that exercises the changed contract, then add broader
integration evidence in proportion to risk.

### Shared kernel code

1. build and test on `x86_64`;
2. commit the shared change there;
3. rebase `arm64`;
4. run the corresponding ARM64 path;
5. report skips and unrun targets explicitly.

### Architecture code

Run the focused target for that architecture and one normal boot target.
Changes to interrupt, TLB, context-switch or cache code need SMP/runtime
coverage; compilation alone does not exercise ordering.

### Remote runqueue wakeup and RCU

Run both normal architecture gates:

```sh
make test-x86_64
make test-arm64-limine
```

The `remote_enqueue` oracle requires at least two online CPUs, execution on the
configured target CPU, a nonzero reschedule-IPI counter, and the canonical
`[selftest] 'remote_enqueue' PASSED` marker. It does not accept eventual worker
completion alone.

The ARM64 normal gate additionally requires `[selftest] 'rcu_publish' PASSED`
under SMP2. This is a runtime cross-CPU publication check; this branch does
not include a separate host-side instruction-shape gate.

### Fixed priority and mutex inheritance

Run the normal gates and adjacent stress targets:

```sh
make test-x86_64
make test-x86_64-sched-stress
make test-arm64
make test-arm64-limine
make test-arm64-limine-sched-stress
```

The `priority` selftest requires exact high/equal/equal/promoted-low ordering,
promotion of an already-linked runqueue node, a single-mutex inversion boost
and restoration of base priority. Normal x86_64 and ARM64 Limine harnesses
require `[selftest] 'priority' PASSED`; stress targets cover adjacent
preemption/runqueue locking rather than replacing that oracle.

Direct ARM64 matters because it constructs static boot, idle, kernel and user
TCBs outside `thread_create_common()`. It verifies common priority
initialization and the idle fallback.

After changing `struct thread`, verify an incremental build as well as a clean
one. x86_64 NASM and ARM64 assembly consume generated `THREAD_FPU_STATE`
offsets; a stale switch object can make FPU save overwrite the runqueue node
even when the C scheduler is correct.

### Module loader

Run positive ABI/execution, missing-entry rollback, and unresolved-symbol/init
negative cases.  A new relocation form needs an object-level check proving the
fixture contains it and a runtime check proving it executes.

### Storage

Run the memory-backed parser/registry tests and the architecture's real virtio
target.  Error-handling changes need the negative image target where one
exists.

### Test harness

When changing a grep expression, timeout, marker or fixture, deliberately show
that the harness fails when its required event is absent.  Otherwise the
harness can become permanently green without testing the kernel.

## Determinism and timing

QEMU scripts use bounded polling and serial markers.  Sources of flakiness
include:

- TCG performance variance;
- tests whose completion depends only on wall-clock delay;
- shared serial log paths;
- stale images or logs;
- non-unique markers;
- races between a final log write and harness termination.

Current scripts reduce these risks by removing logs first, trapping QEMU
cleanup, waiting in short intervals, and requiring multiple markers for complex
flows.

Future tests should prefer kernel tick/round bounds over host time and print
the configured rounds, CPU count and failure observation.  Parallel invocations
must use distinct build/log directories before the suite can safely run them
concurrently.

## Linked kernel-thread argument gate

`make test-x86_64-task-params` uses `conf/test-x86_64-params.conf` and the
`test_task_params.elf` payload. Two instances parse distinct integer, boolean
and string arguments into stack-local structures and check them after timer
handoffs. The host requires `[task-args] params_a PASSED`,
`[task-args] params_b PASSED` and `[task-args] params_bad rejected`, plus all
boot phases; explicit fixture failure or kernel exceptions fail the gate.
The third instance receives an invalid integer. This proves private argv/local
configuration for the same x86_64 image, not writable global isolation,
independent address spaces, complete allocator fault coverage or ARM64 ET_EXEC.

## Gaps and priorities

The highest-value testing gaps are:

1. longer focused TLB/scheduler runs are not separate default CI gates;
2. ARM64 direct-boot and LaFS paths are outside the hosted default workflow;
3. early unit fixtures still share subsystem globals rather than private
   parser/mount instances;
4. there is little malformed-ELF and malformed-LaFS input-space coverage;
5. no hardware-backed test distinguishes QEMU assumptions from real platform
   behaviour;
6. coverage and sanitiser-style host instrumentation are not available for
   freestanding kernel code;
7. RISC-V is a conditional placeholder rather than a maintained third-arch
   runtime gate.

For a teaching kernel, the next improvement should be a periodic full matrix
and stronger failure propagation, not a large number of shallow smoke targets.
Each new test should defend a named invariant and have an oracle stronger than
“the kernel did not crash.”

## Runtime test payload ownership

On x86_64, `priority`, `remote_enqueue`, `rcu_publish` and `rcu_stress` live in
`module/tests/`, each built as `test_<name>.mo`. Their former kernel registration
functions and implementations are removed. `test*.conf` names each payload with
`module=`; the test image allowlist and explicit compatibility image deliver them.
The early VMA/CPIO/LaFS/block-device/user-loader checks remain built in, as does
the registry transaction selftest. Normal base images deliver none of these
runtime payloads.

Each module's `selftest_init()` only registers its descriptor. Configuration and
`prepare()` still run before scheduling the configured tests. Workers are created
outside timer context, `start()` enqueues them, and the original tick/done/passed
callbacks retain their ordering, IPI-count, grace-period and timeout oracles.
Priority preparation now discards every unqueued worker on partial allocation or
priority-validation failure. Once workers start, a timeout is a failed boot-test
run; it is not a general cancellation or forced worker-destruction mechanism.

Descriptors, callbacks, static state and RCU metric nodes borrow the loaded
module storage for the rest of the boot. Payloads are not unloaded after a result.
Modules must match kernel headers, CPU/thread layouts and feature flags; this is
not a version-independent ABI. Required exports include the mutex operations,
reschedule counter, allocation/free and formatting helpers, plus RCU synchronization,
state and metrics used by the current inline interface. Module atomic counters
use the existing kernel helper; its ordering is at least as strong as the original
counter operations. The RCU gates require `CONFIG_RCU=1`.

`make test-x86_64` checks produced ELF symbols for all four payloads and absence
of their implementations in the kernel, then requires actual Limine payload
loading and the canonical results. The focused RCU gate also requires its payload
source marker and the completed-round/blocked-reader/reclamation result.
`make test-x86_64-runtime-modules` additionally omits all four payloads during a
real boot: each must be diagnosed as missing/unregistered and none may report
PASSED. The existing framework may continue boot after a missing test; a running
banner is therefore not a substitute for the required test results. ARM64 branch
integration and hardware evidence are outside this x86_64 migration checkpoint.
