#!/usr/bin/env bash

set -euo pipefail

serial_log="build/test-arm64-limine-serial.log"
qemu_log="build/test-arm64-limine-qemu.log"
expected_failed_module="${ARM64_EXPECT_FAILED_MODULE:-}"
expected_parked_aps="${ARM64_EXPECT_PARKED_APS:-}"
expected_gic_aps="${ARM64_EXPECT_GIC_APS:-}"
expected_online_aps="${ARM64_EXPECT_ONLINE_APS:-}"
expected_sgi_acks="${ARM64_EXPECT_SGI_ACKS:-}"
expected_tlb_remap="${ARM64_EXPECT_TLB_REMAP:-}"
expected_tlb_remap_rounds="${ARM64_EXPECT_TLB_REMAP_ROUNDS:-}"
expected_smp_task_cpus="${ARM64_EXPECT_SMP_TASK_CPUS:-}"
expected_module_rollback="${ARM64_EXPECT_MODULE_ROLLBACK:-}"
expected_remote_enqueue="${ARM64_EXPECT_REMOTE_ENQUEUE:-}"
expected_rcu="${ARM64_EXPECT_RCU:-}"
qemu_smp="${ARM64_QEMU_SMP:-1}"
mkdir -p build
rm -f "$serial_log" "$qemu_log"

# The ISO follows Limine's native EFI boot-partition layout and auto-boots the
# sole entry. Stop as soon as the complete module -> EL0 -> module chain is
# visible in the serial log.
qemu-system-aarch64 \
	-M virt,gic-version=3 \
	-cpu cortex-a57 \
	-m 512M \
	-smp "$qemu_smp" \
	-drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
	-cdrom build/LaOS-arm64-limine.iso \
	-display none \
	-serial "file:$serial_log" \
	-monitor none \
	-no-reboot \
	>"$qemu_log" 2>&1 &
qemu_pid=$!

cleanup()
{
	kill "$qemu_pid" 2>/dev/null || true
	wait "$qemu_pid" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 80); do
	if [[ -f "$serial_log" ]] &&
		grep -aFq '[module-abi] relocation + data + bss, PASS' "$serial_log" &&
		grep -aFq '[module-foo] started: count=2 tick=10' "$serial_log" &&
		grep -aFq 'M3c: ELF -> EL0 -> SVC write -> exit, DONE' "$serial_log" &&
		grep -aFq '[module-foo] resumed after timeout' "$serial_log"; then
		if grep -aFq '[PANIC ' "$serial_log"; then
			break
		fi
		if [[ -n "$expected_failed_module" ]] &&
			! grep -aFq "[task] failed to launch bad from $expected_failed_module (type=1)" "$serial_log"; then
			sleep 0.5
			continue
		fi
		if [[ -n "$expected_parked_aps" ]] &&
			! grep -aFq "[smp] APs parked: $expected_parked_aps/$expected_parked_aps" "$serial_log"; then
			sleep 0.5
			continue
		fi
		if [[ -n "$expected_gic_aps" ]] &&
			! grep -aFq "[smp] AP GIC ready: $expected_gic_aps/$expected_gic_aps" "$serial_log"; then
			sleep 0.5
			continue
		fi
		if [[ -n "$expected_online_aps" ]] &&
			! grep -aFq "[smp] AP online: $expected_online_aps/$expected_online_aps" "$serial_log"; then
			sleep 0.5
			continue
		fi
		if [[ -n "$expected_sgi_acks" ]] &&
			! grep -aFq "$expected_sgi_acks APs per round" "$serial_log"; then
			sleep 0.5
			continue
		fi
		if [[ -n "$expected_tlb_remap" ]] &&
			! grep -aFq "[tlb] TLB remap visible: $expected_tlb_remap/$expected_tlb_remap" "$serial_log"; then
			sleep 0.5
			continue
		fi
		if [[ -n "$expected_tlb_remap_rounds" ]] &&
			! grep -aFq "rounds=$expected_tlb_remap_rounds" "$serial_log"; then
			sleep 0.5
			continue
		fi
		if [[ -n "$expected_smp_task_cpus" ]]; then
			missing=0
			IFS=',' read -r -a _smp_probe_cpus <<< "$expected_smp_task_cpus"
			for cpu in "${_smp_probe_cpus[@]}"; do
				if ! grep -aFq "[task] queued smp${cpu} from module_smp_probe.mo (type=1)" "$serial_log" ||
					! grep -aFq "[smp-probe] cpu=${cpu}" "$serial_log"; then
					missing=1
					break
				fi
			done
			if [[ "$missing" -ne 0 ]]; then
				sleep 0.5
				continue
			fi
		fi
		if [[ "$expected_failed_module" == "module_bad.mo" ]] &&
			! grep -aFq '[load] unresolved module symbol before allocation: laos_missing_symbol_for_negative_test' "$serial_log"; then
			sleep 0.5
			continue
		fi
		if [[ "$expected_failed_module" == "module_bad.mo" ]] &&
			! grep -aFq '[selftest] selftest_init failed (ret=-1)' "$serial_log"; then
			sleep 0.5
			continue
		fi
		if [[ -n "$expected_module_rollback" ]] &&
			! grep -aFq '[ module] module_alloc rollback:' "$serial_log"; then
			sleep 0.5
			continue
		fi
		if [[ -n "$expected_remote_enqueue" ]] &&
			! grep -aFq "[selftest] 'remote_enqueue' PASSED" "$serial_log"; then
			sleep 0.5
			continue
		fi
		if [[ -n "$expected_rcu" ]] &&
			! grep -aFq "[selftest] 'rcu_publish' PASSED" "$serial_log"; then
			sleep 0.5
			continue
		fi
		if [[ -n "$expected_tlb_remap_rounds" ]]; then
			if ! grep -aFq "[selftest] 'registry' PASSED" "$serial_log" ||
			   ! grep -aFq "[selftest] 'cpu_alive' PASSED" "$serial_log" ||
			   ! grep -aFq "[selftest] 'ipi_delivery' PASSED" "$serial_log" ||
			   ! grep -aFq "[selftest] 'smp_tlb_remap' PASSED" "$serial_log"; then
				sleep 0.5
				continue
			fi
		fi
		echo "  PASS: aarch64 Limine module + EL0 task chain"
		exit 0
	fi
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		break
	fi
	sleep 0.5
done

echo "  FAIL: aarch64 Limine (serial log: $serial_log; qemu log: $qemu_log)"
exit 1
