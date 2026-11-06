#!/usr/bin/env bash
# Multiple ET_EXEC instances parse private argv into stack-local configs.
set -euo pipefail

serial_log=build/test-x86_64-task-params-serial.log
qemu_log=build/test-x86_64-task-params-qemu.log
mkdir -p build
rm -f "$serial_log" "$qemu_log"
"${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}" \
	-accel "${X86_64_QEMU_ACCEL:-tcg}" -machine q35 \
	-cdrom build/LaOS.iso -m 2G -smp 1 -net none \
	-no-reboot -no-shutdown -display none -monitor none \
	-serial "file:$serial_log" >"$qemu_log" 2>&1 &
qemu_pid=$!
cleanup()
{
	kill "$qemu_pid" 2>/dev/null || true
	wait "$qemu_pid" 2>/dev/null || true
}
trap cleanup EXIT

limit=$((${X86_64_QEMU_TIMEOUT:-60} * 10))
for ((i = 0; i < limit; i++)); do
	if [[ -f "$serial_log" ]]; then
		if grep -aEq '\[task-args\].* FAILED|\[PANIC |KERNEL EXCEPTION' "$serial_log"; then
			break
		fi
		if grep -aFq '[task-args] params_a PASSED' "$serial_log" &&
		   grep -aFq '[task-args] params_b PASSED' "$serial_log" &&
		   grep -aFq '[task-args] params_bad rejected' "$serial_log"; then
			python3 script/check_boot_phases.py "$serial_log"
			echo '  PASS: x86_64 ET_EXEC per-instance argv and invalid-argument rejection'
			exit 0
		fi
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		break
	fi
	sleep 0.1
done

echo "  FAIL: ET_EXEC task arguments (serial log: $serial_log; QEMU log: $qemu_log)"
exit 1
