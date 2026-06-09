#!/usr/bin/env bash

set -euo pipefail

module="module/bin-aarch64/module_abi.mo"
readelf_bin="${AARCH64_READELF:-aarch64-linux-gnu-readelf}"

if [[ ! -f "$module" ]]; then
	echo "  FAIL: missing ARM64 ABI module: $module"
	exit 1
fi

relocs="$($readelf_bin -rW "$module")"
sections="$($readelf_bin -SW "$module")"

for reloc in \
	R_AARCH64_ABS64 \
	R_AARCH64_CALL26 \
	R_AARCH64_JUMP26 \
	R_AARCH64_PREL32 \
	R_AARCH64_ADR_PREL_PG_HI21 \
	R_AARCH64_ADD_ABS_LO12_NC \
	R_AARCH64_LDST8_ABS_LO12_NC \
	R_AARCH64_LDST32_ABS_LO12_NC \
	R_AARCH64_LDST64_ABS_LO12_NC; do
	if ! grep -Fq "$reloc" <<<"$relocs"; then
		echo "  FAIL: module ABI fixture lacks $reloc"
		exit 1
	fi
done

if ! grep -Eq '\.bss[[:space:]]+NOBITS' <<<"$sections"; then
	echo "  FAIL: module ABI fixture lacks an allocated BSS section"
	exit 1
fi

echo "  PASS: aarch64 module ABI relocation fixture"
