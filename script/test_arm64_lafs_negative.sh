#!/usr/bin/env bash
#
# test_arm64_lafs_negative.sh — ARM64 LaFS negative tests
#
# Tests:
#   1. Bad superblock magic → error, no mount
#
set -euo pipefail

serial_log="build/test-arm64-lafs-negative/serial.log"
qemu_log="build/test-arm64-lafs-negative/qemu.log"
bad_img="build/test-lafs-bad.img"
mkdir -p build/test-arm64-lafs-negative

rm -f "$serial_log" "$qemu_log"

# Generate a bad disk image (all zeros — invalid magic)
dd if=/dev/zero of="$bad_img" bs=512 count=32 2>/dev/null

qemu-system-aarch64 \
	-machine virt,gic-version=3,highmem-ecam=off \
	-cpu cortex-a57 \
	-kernel bin-aarch64/kernel \
	-device virtio-blk-device,drive=drive0 \
	-drive file="$bad_img",format=raw,if=none,id=drive0 \
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

echo "=== TEST aarch64 LaFS negative (bad superblock) ==="
for _ in $(seq 1 60); do
	if [[ -f "$serial_log" ]]; then
		have_bad=$(grep -c '\[lafs\] bad magic' "$serial_log" 2>/dev/null || true)
		have_mount=$(grep -c '\[lafs\] mounted' "$serial_log" 2>/dev/null || true)

		if [ "$have_bad" -ge 1 ] && [ "$have_mount" -eq 0 ]; then
			if grep -q '\[PANIC ' "$serial_log"; then
				break
			fi
			echo "  PASS: aarch64 LaFS negative (bad magic → error, no mount)"
			exit 0
		fi
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		break
	fi
	sleep 0.5
done

echo "  FAIL: aarch64 LaFS negative (serial log: $serial_log)"
exit 1
