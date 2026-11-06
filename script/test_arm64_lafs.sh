#!/usr/bin/env bash
#
# test_arm64_lafs.sh — ARM64 direct-boot LaFS regression test
#
# 1. Build kernel + LaFS disk image
# 2. Boot QEMU with virtio-blk disk attached
# 3. Verify: LaFS mount, /etc/motd, /etc/version reads
#
set -euo pipefail

serial_log="build/test-arm64-lafs/serial.log"
qemu_log="build/test-arm64-lafs/qemu.log"
disk_img="build/test-lafs.img"
mkdir -p build/test-arm64-lafs

rm -f "$serial_log" "$qemu_log"

# Generate disk image if missing
if [ ! -f "$disk_img" ]; then
	python3 script/mkfs_lafs.py -o "$disk_img"
fi

qemu-system-aarch64 \
	-machine virt,gic-version=3,highmem-ecam=off \
	-cpu cortex-a57 \
	-kernel bin-aarch64/kernel \
	-device virtio-blk-device,drive=drive0 \
	-drive file="$disk_img",format=raw,if=none,id=drive0 \
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

echo "=== TEST aarch64 LaFS regression ==="
for _ in $(seq 1 60); do
	if [[ -f "$serial_log" ]]; then
		# All three must be present and no PANIC
		have_mount=$(grep -c '\[lafs\] mounted:' "$serial_log" 2>/dev/null || true)
		have_motd=$(grep -c '\[lafs\] /etc/motd ' "$serial_log" 2>/dev/null || true)
		have_ver=$(grep -c '\[lafs\] /etc/version:' "$serial_log" 2>/dev/null || true)

		if [ "$have_mount" -ge 1 ] && [ "$have_motd" -ge 1 ] && [ "$have_ver" -ge 1 ]; then
			if grep -q '\[PANIC ' "$serial_log"; then
				break
			fi
			echo "  PASS: aarch64 LaFS (mount + /etc/motd + /etc/version)"
			exit 0
		fi
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		break
	fi
	sleep 0.5
done

echo "  FAIL: aarch64 LaFS (serial log: $serial_log)"
exit 1
