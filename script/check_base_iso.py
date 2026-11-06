#!/usr/bin/env python3
"""Check generated staging files and actual ISO contents after switching modes."""
from pathlib import Path
import shlex
import subprocess
import sys
root = Path(sys.argv[1])
expected = {'boot/kernel', 'boot/initrd.cpio', 'limine-bios.sys',
            'limine-bios-cd.bin', 'limine-uefi-cd.bin', 'limine.conf'}
actual = {str(p.relative_to(root/'iso_root')) for p in (root/'iso_root').rglob('*') if p.is_file()}
assert actual == expected, actual
config = (root/'iso_root/limine.conf').read_text()
assert 'cmdline: boot=base' in config
modules = [line.strip() for line in config.splitlines() if 'module_path:' in line]
assert modules == ['module_path: boot():/boot/initrd.cpio'], modules
result = subprocess.run(['xorriso', '-indev', str(root/'LaOS.iso'), '-find', '/',
                         '-type', 'f', '-exec', 'echo'], check=True, capture_output=True, text=True)
files = {shlex.split(line)[0].lstrip('/') for line in result.stdout.splitlines() if line.strip()}
assert files == expected, files
print('PASS: default base ISO has only kernel, initrd and bootloader files after test mode')
