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
| early synchronous kernel test | boot CPU before scheduling | focused deterministic logic | may only warn unless outer target gates marker |
| registered selftest | kernel after configuration/SMP setup | stateful SMP and subsystem behaviour | serialized, timer-driven, QEMU timing |
| QEMU integration target | host plus full kernel | boot/device/ABI/privilege evidence | emulated hardware and marker-based oracle |
| negative integration target | host plus full kernel | proves selected failures are rejected | limited malformed/fault space |

## Ownership and data flow

`kernel/task_parser.c` parses `@test` text into
`struct selftest_directive`.  `kernel/task_conf.c` owns the directive list.
The parser does not call test implementations.

`kernel/selftest.c` owns:

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
| `make test-x86_64-multiuser` | x86_64 EL0 | at least three user-task exits |
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
does not exist.  Likewise, ARM64 targets may skip on an x86_64-only branch.
A skipped target is not runtime evidence for that architecture.

## Hosted CI matrix

The default hosted workflow is narrower than the maintained target matrix:

| CI job/step | x86_64 branch | ARM64 branch |
| --- | --- | --- |
| documentation checker | runs | runs |
| `make test-x86_64` | runs | runs |
| `make test-x86_64-lafs` | runs | runs |
| `make test-arm64-limine` | skips without ARM directory | runs |
| rollback/negative/stress/multi-user targets | not in default workflow | not in default workflow |
| ARM64 direct boot and LaFS | not in default workflow | not in default workflow |

This makes the default CI practical, but changes to uncovered areas require
explicit local/maintainer evidence.  Periodic or release workflows would be a
natural place for the full focused matrix.

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

## Gaps and priorities

The highest-value testing gaps are:

1. default CI does not gate module rollback/negative or scheduler/TLB stress;
2. ARM64 direct-boot and LaFS paths are outside the hosted default workflow;
3. early synchronous unit failures are not uniformly promoted to target
   failure;
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
