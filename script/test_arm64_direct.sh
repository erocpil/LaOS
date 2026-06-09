#!/usr/bin/env bash

set -euo pipefail

serial_log="build/test-arm64/serial.log"
qemu_log="build/test-arm64/qemu.log"
mkdir -p build/test-arm64
rm -f "$serial_log" "$qemu_log"

qemu-system-aarch64 \
	-machine virt,gic-version=3,highmem-ecam=off \
	-cpu cortex-a57 \
	-kernel bin-aarch64/kernel \
	-netdev user,id=u1 \
	-device e1000,netdev=u1 \
	-m 512M -smp 1 \
	-no-reboot -no-shutdown \
	-display none -serial "file:$serial_log" \
	>"$qemu_log" 2>&1 &
qemu_pid=$!

cleanup()
{
	kill "$qemu_pid" 2>/dev/null || true
	wait "$qemu_pid" 2>/dev/null || true
}
trap cleanup EXIT

echo "=== TEST aarch64 direct boot (TCG) ==="
for _ in $(seq 1 60); do
	if [[ -f "$serial_log" ]] &&
		grep -q 'M3c: ELF -> EL0 -> SVC write -> exit, DONE' "$serial_log" &&
		grep -q 'Detected network controller: Ethernet' "$serial_log"; then
		if grep -q '\[PANIC ' "$serial_log"; then
			break
		fi
		echo "  PASS: aarch64 direct boot"
		exit 0
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		break
	fi
	sleep 0.5
done

echo "  FAIL: aarch64 direct boot (serial log: $serial_log)"
exit 1
