# Getting Started

This guide describes the shortest reproducible path from a fresh checkout to a
tested LaOS image. Commands are run from the repository root.

For the complete test-target index and links to detailed methods, configuration
and subsystem documentation, start with the [LaOS test guide](testing-guide.md).

## Supported development environment

The automated gates run on Ubuntu with GCC/binutils, Python 3, QEMU, xorriso,
NASM and an AArch64 cross compiler. LaOS is primarily validated under QEMU; real
hardware support is not a release criterion.

Install the host packages:

```sh
sudo apt-get update
sudo apt-get install -y \
    build-essential git python3 nasm xorriso \
    qemu-system-x86 qemu-system-arm qemu-efi-aarch64 \
    gcc-aarch64-linux-gnu
```

Initialize the repository dependencies:

```sh
git submodule update --init --recursive
bash third_party/limine-c-template/kernel/get-deps
```

The CI workflow also downloads and builds the current Limine binary release
under `third_party/limine-c-template/limine-binary/`. If that directory is
missing, follow the same commands in `.github/workflows/build.yml`.

## x86_64: first build and test

The default architecture is x86_64:

```sh
make
make test-x86_64
make test-x86_64-lafs
make test-x86_64-rcu-stress
```

`make` produces a base-initrd image without runtime test payloads. The test targets
explicitly rebuild a test-mode image, combining the base archive with a separate
Limine `test.conf` and test fixtures. Run `make` again before launching the normal
image. `INITRD_TASK_CONF` selects the base configuration;
`X86_64_TEST_CONF` selects `conf/test.conf` or a `conf/test-x86_64-*.conf` fixture.

Expected gates include:

- `PASS: x86_64 (boot)`
- priority/PI, registry, remote-enqueue, RCU-publication, CPU-alive, IPI,
  SMP TLB and FPU selftests
- `PASS: x86_64 LaFS (real virtio)`

`make run` enables the e1000 device and expects a host TAP interface. For a
headless serial console:

```sh
sudo ./script/setup_host_net.sh up
make run HEADLESS=1
sudo ./script/setup_host_net.sh down
```

The TAP setup changes host networking and is not needed for the test targets.

## ARM64

ARM64 implementation files live on the `arm64` branch under
`kernel/arch/aarch64/`. Switch branches before running ARM64 targets:

```sh
git switch arm64
make test-arm64
make test-arm64-limine
make test-arm64-lafs
```

The Limine path requires an AAVMF firmware image, normally installed as
`/usr/share/AAVMF/AAVMF_CODE.fd` by `qemu-efi-aarch64`.

Useful focused gates include:

```sh
make test-arm64-limine-smp-tlb
make test-arm64-limine-fpu
make test-arm64-limine-sched-stress
make test-arm64-limine-multiuser
```

`make test-arm64-limine` 固定以 2 个 vCPU 运行。除模块与 EL0 链外，它还要求：

```text
[smp] AP online: 1/1 (global=2/2)
[selftest] 'remote_enqueue' PASSED
[selftest] 'rcu_publish' PASSED
```

`remote_enqueue` 的详细结果还应显示 `target_cpu=1 observed_cpu=1` 和非零
`reschedule_ipis`。这同时证明 BSP 在 AP online 后动态入队、SGI 被 CPU1
接收，以及 worker 获得实际运行机会。

本分支的 ARM64 构建目标使用 GNU 交叉工具链，验证流程面向 Linux 主机。
macOS Apple Silicon 上的 LLVM/LLD ARM64 流程由 `arm64` 分支维护；direct
boot 与 Limine boot 是不同启动契约，不能互相替代。ARM64 环境与验证边界见
[ARM64 开发环境](process/arm64-dev-setup.md)。

QEMU 在 Apple Silicon 上通常使用 TCG。QEMU 结果可以证明当前模拟器中的
启动、模块、调度、GIC 和有限内存序路径，但不能替代真实 ARM 硬件对缓存维护、
中断拓扑、性能或硬件特有弱内存行为的验证。

## Before submitting a change

For shared code, follow the [branch strategy](process/branch-strategy.md):
validate and commit on x86_64 first, then rebase ARM64 and run its relevant
gate.

```sh
bash script/check_doc_links.sh
make test-task-conf-v1
make test-x86_64
make test-x86_64-lafs
```

Run `make help` for the full target list. `make test-all` is broad but does not
replace focused Limine, negative and stress targets.

## Troubleshooting

### Limine files are missing

Reinitialize submodules and follow the Limine download step in the CI workflow.
Do not commit generated Limine binaries.

### QEMU times out

Most tests intentionally terminate QEMU through a timeout after checking serial
markers. Inspect `build/serial.log` and the matching `build/*qemu.log` before
treating exit status 124 as a kernel failure.

### KVM is unavailable

The x86_64 commands can run under software emulation, but will be slower. Check
that `/dev/kvm` is accessible if a local run is unexpectedly slow.

### ARM64 firmware cannot be opened

Install `qemu-efi-aarch64` or pass the correct AAVMF path used by the relevant
script/Make target. Distribution filenames differ; the CI workflow documents
the expected Ubuntu layout.

### A selftest is reported as missing

An `@test` directive must name a registered built-in test or provide the
matching `.mo` module. Validate the configuration with
`make test-task-conf-v1` and consult [the task DSL](task-conf-dsl.md).
