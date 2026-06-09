# Testing LaOS

LaOS tests are most useful when treated as layers of evidence rather than one
binary pass/fail signal.  This tutorial shows how to choose and interpret those
layers.

## The three test layers

```text
host checks
  syntax, documentation, fixtures, object shape
        |
        v
in-kernel tests
  data structures, parser logic, SMP/FPU/TLB behaviour
        |
        v
QEMU integration tests
  boot protocol, devices, modules, user transition, serial contract
```

No layer subsumes the others.  A host checker cannot prove that relocated code
executes.  A memory-backed filesystem test cannot prove virtio DMA.  A boot
banner cannot prove a negative error path.

## 1. Start with discovery

List the maintained targets:

```sh
make help
```

Before a full build, run the inexpensive repository checks:

```sh
bash script/check_doc_links.sh
make test-task-conf-v1
```

The documentation checker validates local Markdown links, anchors, selected
source references and Make targets.  The task configuration checker validates
the checked-in DSL v1 fixtures at host level.

The host DSL script is deliberately a structural checker, not the kernel
parser itself.  Parser changes should be tested against both the script's
accepted grammar and a boot that consumes the configuration.

## 2. Understand synchronous unit-style tests

Some focused tests execute during early kernel initialisation, before normal
task scheduling:

- VMA behaviour in `kernel/test_vma.c`;
- CPIO parsing in `kernel/test_cpio.c`;
- LaFS parsing in `kernel/test_lafs.c`;
- block-device registry behaviour in `kernel/test_block_device.c`.

These tests use controlled in-memory fixtures and produce direct PASS/FAIL
diagnostics.  They are good for localising algorithms and boundary cases.

On x86_64, the block-device test registry is reset afterward so its stub does
not hide a real virtio device.  This teardown is part of the test isolation
contract.

Early-test failures currently emit warnings and boot can continue.  Therefore
the surrounding QEMU target must explicitly grep the relevant marker if that
test is intended to be a release gate.  A successful later boot marker alone
does not imply every early unit test passed.

## 3. Configure registered selftests

Longer or hardware-coupled tests use the selftest framework in
`kernel/selftest.c`.  A test supplies a `struct selftest`:

```c
struct selftest {
    const char *name;
    void (*configure)(const char *key, const char *value);
    int  (*prepare)(void);
    void (*start)(void);
    void (*tick)(void);
    bool (*done)(void);
    bool (*passed)(void);
};
```

Enable one with a task configuration directive:

```text
@test ipi_delivery module=test_ipi_delivery.mo rounds=50 timeout_ticks=500
@test remote_enqueue timeout_ticks=200
```

The boot flow is:

```text
parse @test into a directive record
  -> load module= payload, if present
  -> payload selftest_init registers the test
  -> apply key/value configuration
  -> prepare resources
  -> start first configured test
  -> timer/main-loop ticks drive completion
```

The parser only creates records.  It does not know individual test names or
invoke test code.  This separation lets a new selftest module be added without
changing the DSL parser.

Unknown test names are diagnosed when directives are applied.  Unknown
per-test keys are handled by each test's `configure` callback, so every test
must define its own validation policy.

## 4. Respect the selftest phases

Use the phases according to execution context:

- `configure`: consume trusted string key/value pairs;
- `prepare`: allocate workers and other resources in ordinary context;
- `start`: publish/start work after CPUs are online;
- `tick`: make bounded progress when the framework is driven;
- `done`: report completion;
- `passed`: report the final result.

The helper API separates worker creation from enqueue:

- `selftest_create_worker()` in `prepare`;
- `selftest_start_worker()` in `start`;
- `selftest_discard_worker()` on prepare failure.

Tests run serially.  When one completes, the framework prints a canonical
marker and starts the next:

```text
[selftest] 'name' PASSED
```

Serial execution avoids interference between tests that temporarily own IPI
callbacks or shared hardware state.  It also means one hung test prevents later
tests from starting, so every asynchronous test should have a timeout or
bounded completion rule.

If `passed` is absent, completion is treated as success.  For a real gate,
provide an explicit `passed` callback.

## 5. Run the normal architecture smoke tests

For x86_64:

```sh
make test-x86_64
```

This builds the ISO, boots QEMU with four CPUs, requires the main boot marker,
and gates priority/mutex-inheritance ordering, registry, remote enqueue, RCU
publication, CPU-alive, IPI-delivery, SMP TLB-remap and FPU-context selftests.

For ARM64, two boot paths serve different purposes:

```sh
make test-arm64
make test-arm64-limine
```

`test-arm64` is a direct-kernel boot that checks the EL0/SVC path and e1000
discovery. `test-arm64-limine` runs with two CPUs and exercises UEFI/Limine
boot, relocatable module ABI behaviour, module parameters, a user task,
priority/mutex-inheritance ordering, remote AP wakeup, RCU publication and
resumed scheduling.

Do not substitute one for the other when changing boot handoff or module
loading; their evidence differs.

### Verify fixed priorities and priority inheritance

The normal x86_64 and ARM64 Limine gates enable:

```text
@test priority timeout_ticks=500
```

Successful detail:

```text
[priority] order high=1 equal-a=2 equal-b=3 low=4
[priority] PASSED: boost=1 medium_before_unlock=0 restored=48
[selftest] 'priority' PASSED
```

This proves strict selection, FIFO rotation among equals, migration of an
already-enqueued thread, single-hop donation and restoration. It does not
prove starvation freedom, transitive inheritance or load balancing.

After scheduler, TCB or context-switch changes, also run:

```sh
make test-x86_64-sched-stress
make test-arm64
make test-arm64-limine-sched-stress
```

Direct ARM64 covers separately constructed static TCBs. Stress gates exercise
SMP preemption and runqueue locking but have different result oracles from the
exact priority test.

## 6. Select focused integration targets

Storage:

```sh
make test-x86_64-lafs
make test-arm64-lafs
make test-arm64-lafs-negative
```

Module failure handling:

```sh
make test-x86_64-rollback
make test-x86_64-negative
make test-arm64-limine-rollback
make test-arm64-limine-negative
```

SMP, scheduling and user processes:

```sh
make test-x86_64-smp-tlb
make test-x86_64-sched-stress
make test-x86_64-rcu-stress
make test-x86_64-multiuser
make test-arm64-limine-smp-park
make test-arm64-limine-smp-tlb
make test-arm64-limine-fpu
make test-arm64-limine-sched-stress
make test-arm64-limine-multiuser
```

Focused targets rebuild with a matching file under `conf/` and check exact
serial markers.  They are preferable to manually booting a default image when
the change affects a specific failure or concurrency contract.

`make test-all` is a convenient aggregate, but its name does not mean every
focused target above.  Read its dependencies in `Makefile` before using it as
a completeness claim.

## 7. Read a QEMU result critically

Most runtime tests:

1. delete an old serial log;
2. start QEMU with a timeout or background process;
3. wait for required markers;
4. reject a panic marker;
5. print PASS or show the log path.

The required markers are the executable contract.  When adding a test, choose
markers that prove the operation itself:

- bad: only `LaOS is running`;
- better: filesystem mounted;
- stronger: mount plus known contents from the attached image;
- negative: expected error present and success marker absent.

Check both sides of a negative result.  For example, the ARM64 bad-LaFS test
requires the bad-magic diagnostic and forbids a mounted marker.

Avoid accepting timeout as success unless all required markers were already
verified.  A kernel that merely stayed alive until timeout has not necessarily
completed the test.

## 8. Match tests to a change

Examples:

| Change | Minimum useful evidence |
| --- | --- |
| Markdown/source-link update | `bash script/check_doc_links.sh` |
| `task.conf` grammar | `make test-task-conf-v1` plus a consuming boot |
| generic LaFS parser | early LaFS test plus one architecture integration target |
| x86_64 virtio PCI | `make test-x86_64-lafs` |
| ARM64 virtio MMIO | `make test-arm64-lafs` and negative target when error handling changes |
| shared module loader | positive, rollback and negative targets on both branches |
| TLB shootdown | architecture SMP TLB stress target |
| scheduler/FPU state | corresponding stress/FPU target on the affected architecture |
| user address-space/task lifetime | normal boot plus multi-user target |

For shared code, validate x86_64 first, then rebase ARM64 and run the relevant
ARM64 target.  A compile on both architectures is necessary but is weaker than
executing both architecture-specific paths.

## 9. Add a new module selftest

1. Create a focused source in `module/`.
2. Implement configuration validation and bounded completion.
3. Export a module-local `selftest_init()` that registers the descriptor.
4. Add its build rule to `module/Makefile`.
5. Add an `@test` directive in a dedicated `conf/` fixture.
6. Add a Make target or extend an appropriate existing gate.
7. Require the canonical selftest PASS marker and reject panic.
8. Add a failure fixture when the feature has meaningful error behaviour.
9. Update the matrix in [Testing architecture](testing-arch.md).

Keep fixtures deterministic.  Randomised stress is useful only if its seed and
failure context are printed so a failure can be reproduced.

## 10. CI expectations

The workflow in `.github/workflows/build.yml` currently runs:

- documentation checks;
- the normal x86_64 smoke target;
- the x86_64 real-LaFS target;
- the normal ARM64 Limine target when the ARM64 architecture directory exists.

Focused rollback, negative, stress, multi-user, ARM64 direct-boot and ARM64
LaFS targets exist locally but are not all part of the default hosted workflow.
When one of those areas changes, run the relevant target explicitly and report
it; a green default workflow does not cover every row of the test matrix.
