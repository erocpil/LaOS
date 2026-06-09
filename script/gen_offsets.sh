#!/bin/bash
set -e

INPUT=$1
TEMP_S=$(mktemp ./tmp_offsets.XXXXXX.s)
TEMP_GAS=$(mktemp ./asm_offsets_gas.XXXXXX.h)
TEMP_NASM=$(mktemp ./asm_offsets_nasm.XXXXXX.inc)
trap 'rm -f "$TEMP_S" "$TEMP_GAS" "$TEMP_NASM"' EXIT

update_if_changed()
{
	local generated=$1
	local destination=$2

	if [ -f "$destination" ] && cmp -s "$generated" "$destination"; then
		return
	fi
	mv "$generated" "$destination"
}

# 1. 编译
gcc -S -O2 -I ../.. -I . "$INPUT" -o "$TEMP_S"

# 2. 生成 GAS 风格 (C Header 格式，用于 .S 文件)
{
    echo "/* Generated for GAS - DO NOT EDIT */"
    echo "#ifndef __ASM_OFFSETS_GAS_H__"
    echo "#define __ASM_OFFSETS_GAS_H__"
    # sed 提取：#define KEY VALUE
	# 修改匹配模式，匹配 #ASM_OFFSET#
	sed -ne "s/^#ASM_OFFSET# \([^ ]*\) [\t$]*\([^ ]*\) \(.*\)/#define \1 \2/p" "$TEMP_S"
    echo "#endif"
} > "$TEMP_GAS"

# 3. 生成 NASM 风格 (NASM %assign 格式，用于 .asm 文件)
{
    echo "; Generated for NASM - DO NOT EDIT"
    # sed 提取： %assign KEY VALUE
    # NASM 中使用 %assign 或 %define 都可以，%assign 处理数值更高效
	sed -ne "s/^#ASM_OFFSET# \([^ ]*\) [\t$]*\([^ ]*\) \(.*\)/%assign \1 \2/p" "$TEMP_S"
} > "$TEMP_NASM"

update_if_changed "$TEMP_GAS" asm_offsets_gas.h
update_if_changed "$TEMP_NASM" asm_offsets_nasm.inc
echo "Dual offset files generated: asm_offsets_gas.h & asm_offsets_nasm.inc"
