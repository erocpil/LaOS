#!/usr/bin/env python3
"""Build an allowlisted base archive; validate with the production kernel parser."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
from mkcpio import write_entry, write_trailer


def canonical(name):
    parts = name.split('/')
    if ':' in name or '..' in parts or '\0' in name:
        raise ValueError(f'invalid archive path: {name}')
    name = '/' + '/'.join(p for p in parts if p not in ('', '.'))
    if name == '/' or len(name.encode()) >= 256:
        raise ValueError('archive path is empty or too long')
    return name


def build(args):
    machine = {'x86_64': 62, 'aarch64': 183}[args.arch]
    entries = {'/conf/task.conf': Path(args.config).read_bytes()}
    metadata = []
    for mapping in args.component:
        name, local = mapping.split('=', 1)
        name = canonical(name)
        if name in entries:
            raise ValueError(f'duplicate archive path: {name}')
        data = Path(local).read_bytes()
        if len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01':
            raise ValueError(f'invalid ELF: {name}')
        kind, target = struct.unpack_from('<HH', data, 16)
        if target != machine or kind not in (1, 2) or struct.unpack_from('<H', data, 52)[0] != 64:
            raise ValueError(f'wrong ELF architecture/type: {name}')
        entries[name] = data
        metadata.append({'path': name, 'elf_type': kind, 'elf_machine': target})
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH', '0'))
    if not 0 <= epoch <= 0xffffffff:
        raise ValueError('SOURCE_DATE_EPOCH is outside newc range')
    output, manifest = Path(args.output), Path(args.manifest)
    if output.resolve() == manifest.resolve():
        raise ValueError('archive and manifest must have different paths')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix='.initrd-') as work:
        candidate = Path(work) / 'archive.cpio'
        with candidate.open('wb') as stream:
            for ino, (name, data) in enumerate(sorted(entries.items()), 1):
                write_entry(stream, name.lstrip('/'), data, mtime=epoch, ino=ino)
            write_trailer(stream, mtime=epoch, ino=len(entries) + 1)
        subprocess.run([str(Path(args.validator).resolve()), str(candidate), str(machine)], check=True)
        details = {item['path']: item for item in metadata}
        record = {'format_version': 1, 'arch': args.arch, 'config': '/conf/task.conf',
                  'archive_sha256': hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  'entries': [dict(path=name, size=len(data), sha256=hashlib.sha256(data).hexdigest(),
                                   **{k: v for k, v in details.get(name, {}).items() if k != 'path'})
                              for name, data in sorted(entries.items())]}
        manifest.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(mode='w', dir=manifest.parent, delete=False) as stream:
            temporary_manifest = Path(stream.name)
            json.dump(record, stream, sort_keys=True, indent=2)
            stream.write('\n')
        try:
            os.replace(candidate, output)
            os.replace(temporary_manifest, manifest)
        finally:
            temporary_manifest.unlink(missing_ok=True)
    print(f'initrd: {output} ({len(entries)} files); manifest: {manifest}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=['x86_64', 'aarch64'], required=True)
    for name in ('config', 'output', 'manifest', 'validator'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--component', action='append', default=[], metavar='ARCHIVE_PATH=LOCAL_FILE')
    args = parser.parse_args()
    try:
        build(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'initrd: {error}\n')


if __name__ == '__main__':
    main()
