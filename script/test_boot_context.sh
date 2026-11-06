#!/usr/bin/env bash
set -euo pipefail
work=$(mktemp -d "${TMPDIR:-/tmp}/laos-boot-context.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work" <<'PY'
import sys, pathlib, struct, importlib.util
sys.dont_write_bytecode=True
root=pathlib.Path(sys.argv[1])
spec=importlib.util.spec_from_file_location('mkcpio','script/mkcpio.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
entries=[
 [('conf/task.conf',b'@version 1\n@module_missing panic\n0 user.elf:init 3 0xc\n'),('task/shared.mo',b'BASE')],
 [('conf/test.conf',b'@version 1\n@test registry\n1 shared.mo:probe 1 0xa\n'),('task/shared.mo',b'TEST'),('task/only-test.mo',b'ONLY')],
 [('task/shared.mo',b'A'),('./task//shared.mo',b'B')],
 [('.',b''),('task',b''),('task/shared.mo',b'DIRS')]]
for i,files in enumerate(entries):
 with (root/f'{i}.cpio').open('wb') as f:
  for name,data in files:
   offset=f.tell(); m.write_entry(f,name,data)
   if name in ('.','task'):
    end=f.tell();f.seek(offset+14);f.write(b'000041ED');f.seek(end)
  m.write_trailer(f)
 # Independent newc offset check, including the trailer and all data bytes.
 raw=(root/f'{i}.cpio').read_bytes(); off=0
 for name,data in files+[('TRAILER!!!',b'')]:
  assert raw[off:off+6]==b'070701'
  namesize=int(raw[off+94:off+102],16); size=int(raw[off+54:off+62],16)
  assert raw[off+110:off+110+namesize]==name.encode()+b'\0'
  start=(off+110+namesize+3)&~3
  assert raw[start:start+size]==data
  off=(start+size+3)&~3
PY
"${HOST_CC:-cc}" -std=gnu11 -g -O1 -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I tests/host/include -I kernel -I kernel/tests \
	tests/host/test_boot_context.c tests/host/host_support.c kernel/boot_files.c kernel/cpio.c \
	kernel/task_conf.c kernel/task_parser.c -o "$work/test_boot_context"
"$work/test_boot_context" "$work/0.cpio" "$work/1.cpio" "$work/2.cpio" "$work/3.cpio"
