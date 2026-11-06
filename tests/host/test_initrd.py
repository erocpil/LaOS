#!/usr/bin/env python3
"""Exercise standalone packaging against real ELF files and kernel parsers."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
sys.path.insert(0, 'script')
from mkcpio import write_entry, write_trailer

arch, validator = sys.argv[1:]
machine = {'x86_64': 62, 'aarch64': 183}[arch]
with tempfile.TemporaryDirectory(prefix='laos-initrd-') as work:
    root = Path(work)
    config = root / 'task.conf'
    original = Path('conf/task-base.conf').read_bytes()
    config.write_bytes(original)
    output, manifest = root / 'initrd.cpio', root / 'manifest.json'
    components = [('user.elf', Path(f'user/bin-{arch}/user.elf')),
                  ('module_foo.mo', Path(f'module/bin-{arch}/module_foo.mo')),
                  ('e1000.mo', Path(f'module/bin-{arch}/e1000.mo'))]
    command = [sys.executable, 'script/build_initrd.py', '--arch', arch,
               '--config', str(config), '--output', str(output), '--manifest', str(manifest),
               '--validator', validator]
    for name, local in components:
        command += ['--component', f'/task/{name}={local}']
    def run(extra=(), success=True):
        result = subprocess.run(command + list(extra), capture_output=True, text=True,
                                env=dict(os.environ, SOURCE_DATE_EPOCH='0'))
        assert (result.returncode == 0) == success, result.stdout + result.stderr
    run()
    baseline = output.read_bytes(), manifest.read_bytes()
    run()
    assert baseline == (output.read_bytes(), manifest.read_bytes())
    raw = baseline[0]
    entries, offset = {}, 0
    while True:
        assert raw[offset:offset+6] == b'070701'
        size = int(raw[offset+54:offset+62], 16)
        namesize = int(raw[offset+94:offset+102], 16)
        name = raw[offset+110:offset+110+namesize-1].decode()
        start = (offset+110+namesize+3) & ~3
        data = raw[start:start+size]
        offset = (start+size+3) & ~3
        if name == 'TRAILER!!!':
            break
        entries['/'+name] = data
    assert set(entries) == {'/conf/task.conf', *('/task/'+n for n, _ in components)}
    assert entries['/conf/task.conf'] == original
    for name, local in components:
        assert entries['/task/'+name] == local.read_bytes()
    record = json.loads(baseline[1])
    assert record['archive_sha256'] == hashlib.sha256(raw).hexdigest()
    for item in record['entries']:
        assert item['size'] == len(entries[item['path']])
        assert item['sha256'] == hashlib.sha256(entries[item['path']]).hexdigest()
    for bad in (original + b'@test registry\n',
                original.replace(b'user.elf', b'absent.elf'),
                original.replace(b'panic', b'skip').replace(b'user.elf', b'absent.elf'),
                original.replace(b'user.elf', b'../user.elf'),
                original.replace(b'user.elf', b'limine:user.elf'),
                original.replace(b'3 0xc', b'1 0xc'), b'@version 1\n'):
        config.write_bytes(bad)
        run(success=False)
        assert baseline == (output.read_bytes(), manifest.read_bytes())
    config.write_bytes(original)
    run(['--component', f'/task/./user.elf={components[0][1]}'], False)
    run(['--component', f'/task/../bad={components[0][1]}'], False)
    run(['--component', f'/task/missing={root / "absent"}'], False)
    for content in (b'\x7fELF', components[0][1].read_bytes()[:18] + b'\xff\xff' + components[0][1].read_bytes()[20:]):
        badelf = root / 'bad.elf'
        badelf.write_bytes(content)
        run(['--component', f'/task/bad.elf={badelf}'], False)
    assert baseline == (output.read_bytes(), manifest.read_bytes())
    # Bypass the Python builder: the production parser must still reject these.
    for badconf in (original + b'@test registry\n', original.replace(b'user.elf', b'absent.elf')):
        badarchive = root / 'bad.cpio'
        with badarchive.open('wb') as stream:
            write_entry(stream, 'conf/task.conf', badconf, mtime=0)
            for name, local in components:
                write_entry(stream, 'task/'+name, local.read_bytes(), mtime=0)
            write_trailer(stream, mtime=0)
        result = subprocess.run([validator, str(badarchive), str(machine)], capture_output=True)
        assert result.returncode != 0
print('PASS: deterministic initrd, byte/hash integrity, reference/type/source rejection and preserved outputs')
