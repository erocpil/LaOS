#!/usr/bin/env bash

set -euo pipefail

iso=${1:-build/LaOS.iso}
serial_log=${2:-build/serial.log}
qemu_log=${3:-build/test-x86_64-qemu.log}
debugcon_log=${4:-build/test-x86_64-debugcon.log}
smp=${5:-4}
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
accel=${X86_64_QEMU_ACCEL:-tcg}
timeout_seconds=${X86_64_QEMU_TIMEOUT:-60}

mkdir -p "$(dirname "$serial_log")" "$(dirname "$qemu_log")" \
	"$(dirname "$debugcon_log")"
rm -f "$serial_log" "$qemu_log" "$debugcon_log"

"$qemu" \
	-accel "$accel" \
	-machine q35 \
	-cdrom "$iso" \
	-m 2G \
	-smp "$smp" \
	-net none \
	-no-reboot \
	-no-shutdown \
	-display none \
	-monitor none \
	-serial "file:$serial_log" \
	-chardev "file,id=con0,path=$debugcon_log" \
	-device isa-debugcon,iobase=0xe9,chardev=con0 \
	>"$qemu_log" 2>&1 &
qemu_pid=$!

cleanup()
{
	kill "$qemu_pid" 2>/dev/null || true
	wait "$qemu_pid" 2>/dev/null || true
}
trap cleanup EXIT

expected=(
	registry priority remote_enqueue rcu_publish ipi_delivery cpu_alive
	smp_tlb_remap fpu_context sched_stress rcu_stress
)
all_markers_present()
{
	[ -f "$serial_log" ] || return 1
	grep -aFq '[boot-config] purpose=0 selected base-initrd:/conf/task.conf' "$serial_log" || return 1
	grep -aFq '[boot-config] purpose=1 selected limine-tests:/conf/test.conf' "$serial_log" || return 1
	grep -aFq 'LaOS is running' "$serial_log" || return 1
	for payload in priority remote_enqueue rcu_publish ipi_delivery cpu_alive \
			tlb fpu_context sched_stress rcu_stress; do
		grep -aFq "from limine-tests:/task/test_${payload}.mo" "$serial_log" || return 1
	done
	for test_name in "${expected[@]}"; do
		grep -aFq "[selftest] '$test_name' PASSED" "$serial_log" || return 1
	done
}

elapsed_tenths=0
limit_tenths=$((timeout_seconds * 10))
while (( elapsed_tenths < limit_tenths )); do
	if all_markers_present; then
		python3 script/check_boot_phases.py "$serial_log"
		echo "  PASS: x86_64 (boot)"
		for test_name in "${expected[@]}"; do
			echo "  PASS: x86_64 ($test_name)"
		done
		exit 0
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		break
	fi
	sleep 0.1
	((elapsed_tenths += 1))
done

echo "  FAIL: x86_64 QEMU did not produce all required markers within ${timeout_seconds}s"
echo "  serial log: $serial_log"
echo "  QEMU log: $qemu_log"
tail -80 "$serial_log" 2>/dev/null || true
tail -40 "$qemu_log" 2>/dev/null || true
exit 1
