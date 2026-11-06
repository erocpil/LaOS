#!/usr/bin/env python3
"""Inspect produced ELF files to rule out a built-in runtime-test fallback."""
import shlex
import subprocess
import sys
from pathlib import Path
nm = shlex.split(sys.argv[1])
arch = sys.argv[2]
names = ('priority', 'remote_enqueue', 'rcu_publish', 'rcu_stress')
def symbols(path):
    output = subprocess.check_output(nm + ['--defined-only', str(path)], text=True)
    return {line.split()[-1] for line in output.splitlines() if line.split()}
kernel = symbols(Path(f'bin-{arch}/kernel'))
for name in names:
    forbidden = {f'test_{name}_init', f'{name}_test', f'{name}_prepare', f'{name}_tick'}
    assert not forbidden & kernel, forbidden & kernel
    payload = Path(f'module/bin-{arch}/test_{name}.mo')
    defined = symbols(payload)
    assert 'selftest_init' in defined and f'{name}_test' in defined, payload
print('PASS: four runtime tests live in loadable modules, without kernel implementations')
