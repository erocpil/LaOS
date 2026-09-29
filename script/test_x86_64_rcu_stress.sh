#!/usr/bin/env bash

set -uo pipefail

build_dir="build"
serial_log="$build_dir/test-x86_64-rcu-stress-serial.log"
qemu_log="$build_dir/test-x86_64-rcu-stress-qemu.log"
debugcon_log="$build_dir/test-x86_64-rcu-stress-debugcon.log"
qemu_bin="${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}"
qemu_machine="${QEMU_MACHINE:-q35}"
qemu_smp="${X86_64_QEMU_SMP:-4}"
timeout_seconds="${X86_64_RCU_STRESS_TIMEOUT:-20}"
max_attempts=2
poll_interval=0.5
max_polls=$((timeout_seconds * 2))
qemu_pid=""

stop_qemu()
{
	if [[ -n "$qemu_pid" ]]; then
		kill "$qemu_pid" 2>/dev/null || true
		wait "$qemu_pid" 2>/dev/null || true
		qemu_pid=""
	fi
}
trap stop_qemu EXIT

mkdir -p "$build_dir"
echo "=== TEST x86_64 RCU stress (smp=$qemu_smp) ==="

for attempt in $(seq 1 "$max_attempts"); do
	stop_qemu
	rm -f "$serial_log" "$qemu_log" "$debugcon_log"
	: >"$serial_log"
	: >"$debugcon_log"

	"$qemu_bin" \
		-machine "$qemu_machine" -cdrom "$build_dir/LaOS.iso" \
		-m 2G -smp "$qemu_smp" -net none \
		-no-reboot -no-shutdown \
		-display none -serial "file:$serial_log" \
		-chardev "file,id=con0,path=$debugcon_log" \
		-device isa-debugcon,iobase=0xe9,chardev=con0 \
		>"$qemu_log" 2>&1 &
	qemu_pid=$!

	for _ in $(seq 1 "$max_polls"); do
		if grep -aFq '[PANIC ' "$serial_log"; then
			break
		fi
		if grep -aFq "[selftest] 'rcu_stress' PASSED" "$serial_log"; then
			echo "  PASS: x86_64 rcu_stress"
			exit 0
		fi
		if ! kill -0 "$qemu_pid" 2>/dev/null; then
			break
		fi
		sleep "$poll_interval"
	done

	stop_qemu
	if [[ -s "$serial_log" || "$attempt" -eq "$max_attempts" ]]; then
		break
	fi
	echo "  WARN: QEMU produced no serial output; retrying ($attempt/$max_attempts)"
done

echo "  FAIL: x86_64 rcu_stress"
echo "        serial log: $serial_log"
echo "        qemu log: $qemu_log"
echo "        debugcon log: $debugcon_log"
if [[ ! -s "$serial_log" ]]; then
	echo "        QEMU produced no serial output after $max_attempts attempts"
fi
exit 1
