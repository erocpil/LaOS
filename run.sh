#!/usr/bin/env bash

set -euo pipefail

mode="${1:-manual}"
firmware="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.fd}"

case "$mode" in
	manual)
		if [[ ! -f "$firmware" ]]; then
			echo "AAVMF firmware not found: $firmware" >&2
			echo "Set AAVMF_CODE=/path/to/AAVMF_CODE.fd and retry." >&2
			exit 1
		fi

		make iso-limine-arm64

		cat <<'EOF'

Manual verification:
  1. If Limine shows the boot-volume warning, press Enter.
  2. At the Limine boot menu, press Enter again.
  3. Check the terminal for both markers:
       [task] queued user0 from user.elf (type=3)
       [module-foo] started: count=2 tick=10
  4. Close QEMU with Ctrl+C in this terminal.

EOF

		qemu-system-aarch64 \
			-M virt,gic-version=3 \
			-cpu cortex-a57 \
			-m 512M \
			-drive "if=pflash,format=raw,readonly=on,file=$firmware" \
			-cdrom build/LaOS-arm64-limine.iso \
			-device qemu-xhci \
			-device usb-kbd \
			-display gtk \
			-serial stdio \
			-monitor none \
			-no-reboot
		;;
	auto)
		make test-arm64-limine
		;;
	rollback)
		make test-arm64-limine-rollback
		;;
	smp)
		# SMP bring-up gate: AP parking, per-CPU preparation and GICR
		# affinity discovery.  Override SMP_CPUS for wider QEMU coverage.
		cpus="${SMP_CPUS:-2}"
		if ! [[ "$cpus" =~ ^[2-9][0-9]*$ ]]; then
			echo "SMP_CPUS must be an integer >= 2" >&2
			exit 2
		fi
		probe_cpus=""
		for ((i = 1; i < cpus && i <= 3; i++)); do
			if [[ -n "$probe_cpus" ]]; then
				probe_cpus+=","
			fi
			probe_cpus+="$i"
		done
		make ARM64_QEMU_SMP="$cpus" ARM64_EXPECT_SMP_TASK_CPUS="$probe_cpus" \
			test-arm64-limine-smp-park
		;;
	smp-tlb)
		# SMP TLB shootdown gate: use a CPU0-only task.conf so APs stay
		# in idle with IRQs enabled and can ack SGI TLB IPIs.
		cpus="${SMP_CPUS:-2}"
		if ! [[ "$cpus" =~ ^[2-9][0-9]*$ ]]; then
			echo "SMP_CPUS must be an integer >= 2" >&2
			exit 2
		fi
		make ARM64_QEMU_SMP="$cpus" test-arm64-limine-smp-tlb
		;;
	direct)
		make kernel ARCH=aarch64 TOOLCHAIN=aarch64-linux-gnu -j"$(nproc)"
		qemu-system-aarch64 \
			-M virt,gic-version=3,highmem=off \
			-cpu cortex-a57 \
			-m 512M \
			-kernel bin-aarch64/kernel \
			-display none \
			-serial stdio \
			-netdev user,id=u1 \
			-device e1000,netdev=u1 \
			-no-reboot
		;;
	*)
		echo "Usage: $0 [manual|auto|rollback|smp|smp-tlb|direct]" >&2
		exit 2
		;;
esac
