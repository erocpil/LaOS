#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_root"

usage()
{
	cat <<'EOF'
Usage: bash run_x86_64.sh [--test|--base] [--timeout SECONDS]

  --test    run the complete positive module/selftest workload (default)
  --base    run only the standalone base initrd workload
  --timeout maximum time to wait for all expected markers

Intentionally failing rollback and loader fixtures remain in their dedicated
make test-x86_64-rollback and make test-x86_64-negative gates.
EOF
}

mode=test
timeout_seconds=300
while [[ $# -gt 0 ]]; do
	case "$1" in
	--test) mode=test ;;
	--base) mode=base; timeout_seconds=60 ;;
	--timeout)
		[[ $# -ge 2 && "$2" =~ ^[1-9][0-9]*$ ]] || {
			echo "error: --timeout requires a positive integer" >&2
			exit 2
		}
		timeout_seconds=$2
		shift
		;;
	-h|--help) usage; exit 0 ;;
	*) echo "error: unknown argument: $1" >&2; usage >&2; exit 2 ;;
	esac
	shift
done

branch="$(git branch --show-current)"
if [[ "$branch" != "x86_64" ]]; then
	echo "error: run this script on the x86_64 branch (current: $branch)" >&2
	echo "       switch branches manually after preserving any local changes" >&2
	exit 1
fi

LLVM_BIN=/opt/homebrew/opt/llvm@22/bin
LLD_BIN=/opt/homebrew/opt/lld@22/bin
export PATH="$LLVM_BIN:$LLD_BIN:$PATH"

required_tools=(
	make bear qemu-system-x86_64 nasm xorriso python3
	"$LLVM_BIN/clang" "$LLVM_BIN/llvm-nm" "$LLD_BIN/ld.lld"
)
for tool in "${required_tools[@]}"; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "error: required tool not found: $tool" >&2
		exit 1
	fi
done

make_args=(
	ARCH=x86_64
	"CC=$LLVM_BIN/clang --target=x86_64-unknown-none-elf"
	"LD=$LLD_BIN/ld.lld"
	"NM=$LLVM_BIN/llvm-nm"
)

mkdir -p "$repo_root/.local"
log_dir="$(mktemp -d "$repo_root/.local/run-x86_64.XXXXXX")"
build_log="$log_dir/build.log"
serial_log="$log_dir/serial.log"
debug_log="$log_dir/debugcon.log"
qemu_log="$log_dir/qemu.log"
serial_pipe="$log_dir/serial.pipe"

echo "Mode: $mode"
echo "Logs for this run: $log_dir"
echo "Cleaning generated build artifacts..."
make clean 2>&1 | tee "$build_log"

echo "Building x86_64 kernel, user programs and modules..."
make "${make_args[@]}" iso X86_64_BOOT_MODE="$mode" \
	2>&1 | tee -a "$build_log"

# Bear's collector needs process-interception privileges that are not always
# available on macOS. Parse deterministic dry runs in each Makefile's real
# working directory instead; this also preserves correct module/user paths.
make -B -n -f kernel.mk "${make_args[@]}" \
	>"$log_dir/compile-kernel.log"
make -B -n -C user ARCH=x86_64 \
	CC="$LLVM_BIN/clang --target=x86_64-unknown-none-elf" \
	LD="$LLD_BIN/ld.lld" \
	>"$log_dir/compile-user.log"
make -B -n -C module ARCH=x86_64 \
	CC="$LLVM_BIN/clang --target=x86_64-unknown-none-elf" \
	LD="$LLD_BIN/ld.lld" \
	>"$log_dir/compile-module.log"
bear parse-sh --input "$log_dir/compile-kernel.log" \
	--output "$repo_root/compile_commands.json" \
	--directory "$repo_root"
bear parse-sh --append --input "$log_dir/compile-user.log" \
	--output "$repo_root/compile_commands.json" \
	--directory "$repo_root/user"
bear parse-sh --append --input "$log_dir/compile-module.log" \
	--output "$repo_root/compile_commands.json" \
	--directory "$repo_root/module"
echo "Compilation database: $repo_root/compile_commands.json"

echo "Starting x86_64 QEMU with TCG and an isolated user-mode e1000 backend."
echo "Serial console: $serial_log"
echo "Debug console:  $debug_log"
echo "QEMU messages:  $qemu_log"

qemu_pid=
tee_pid=
cleanup()
{
	if [[ -n "$qemu_pid" ]] && kill -0 "$qemu_pid" 2>/dev/null; then
		kill "$qemu_pid" 2>/dev/null || true
		wait "$qemu_pid" 2>/dev/null || true
	fi
	if [[ -n "$tee_pid" ]]; then
		kill "$tee_pid" 2>/dev/null || true
		wait "$tee_pid" 2>/dev/null || true
	fi
	rm -f "$serial_pipe"
}
trap cleanup EXIT INT TERM

# A named pipe keeps qemu_pid tied to QEMU while tee mirrors only the serial
# stream. It also works in shells/environments where /dev/fd is restricted.
mkfifo "$serial_pipe"
tee "$serial_log" <"$serial_pipe" &
tee_pid=$!
qemu-system-x86_64 \
	-accel tcg -machine q35 -cdrom "$repo_root/build/LaOS.iso" \
	-m 2G -smp 4 \
	-netdev user,id=u1 \
	-device e1000,netdev=u1,mac=52:54:00:12:34:56 \
	-no-reboot -no-shutdown -display none -monitor none \
	-serial stdio \
	-chardev "file,id=con0,path=$debug_log" \
	-device isa-debugcon,iobase=0xe9,chardev=con0 \
	>"$serial_pipe" 2>"$qemu_log" &
qemu_pid=$!

base_markers=(
	"[boot-config] purpose=0 selected base-initrd:/conf/task.conf"
	"[module-foo] resumed after timeout"
	"[user-exit] init code=0"
)
test_markers=(
	"[boot-config] purpose=1 selected limine-tests:/conf/test.conf"
	"[module-abi] relocation + data + bss, PASS"
	"[E1000] Link is UP"
)
selftests=(
	registry priority remote_enqueue rcu_publish ipi_delivery cpu_alive
	smp_tlb_remap fpu_context sched_stress rcu_stress
)

all_markers_present()
{
	[[ -f "$serial_log" ]] || return 1
	local marker test_name
	for marker in "${base_markers[@]}"; do
		grep -aFq "$marker" "$serial_log" || return 1
	done
	if [[ "$mode" == test ]]; then
		for marker in "${test_markers[@]}"; do
			grep -aFq "$marker" "$serial_log" || return 1
		done
		for test_name in "${selftests[@]}"; do
			grep -aFq "[selftest] '$test_name' PASSED" "$serial_log" || return 1
		done
	fi
}

SECONDS=0
completed=0
while (( SECONDS < timeout_seconds )); do
	if all_markers_present; then
		completed=1
		break
	fi
	if [[ -f "$serial_log" ]] &&
		grep -aEq '\[PANIC |KERNEL EXCEPTION|\[selftest\].* FAILED|\[task-args\].* FAILED' "$serial_log"; then
		echo "error: kernel or test failure detected" >&2
		break
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		break
	fi
	sleep 0.2
done

cleanup
qemu_pid=
tee_pid=

if (( completed == 0 )); then
	echo "FAIL: did not observe every $mode marker within ${timeout_seconds}s" >&2
	echo "Serial log: $serial_log" >&2
	echo "QEMU log:  $qemu_log" >&2
	exit 1
fi

python3 script/check_boot_phases.py "$serial_log"
echo "PASS: x86_64 $mode workload completed with all expected markers."

if [[ "$mode" == test ]]; then
	echo "Building the isolated fixed-address task-parameter scenario..."
	make "${make_args[@]}" iso X86_64_BOOT_MODE=test \
		X86_64_TEST_CONF=conf/test-x86_64-params.conf \
		2>&1 | tee -a "$build_log"
	QEMU_SYSTEM_X86_64=qemu-system-x86_64 \
		X86_64_QEMU_ACCEL=tcg \
		X86_64_QEMU_TIMEOUT="$timeout_seconds" \
		bash script/test_x86_64_task_params.sh \
		2>&1 | tee "$log_dir/task-params-harness.log"
	cp build/test-x86_64-task-params-serial.log \
		"$log_dir/task-params-serial.log"
	cp build/test-x86_64-task-params-qemu.log \
		"$log_dir/task-params-qemu.log"
	echo "PASS: isolated ET_EXEC task-parameter scenario completed."
fi

echo "Logs are saved under $log_dir."
