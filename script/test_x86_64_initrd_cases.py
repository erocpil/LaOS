#!/usr/bin/env python3
"""Boot altered archives without the packer's validation, exercising runtime policy."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
sys.dont_write_bytecode = True
from mkcpio import write_entry, write_trailer

root = Path(sys.argv[1])
user = Path('user/bin-x86_64/user.elf').read_bytes()
worker = Path('module/bin-x86_64/module_foo.mo').read_bytes()
base = b'@version 1\n@module_missing panic\n0 user.elf:init 3 0xc\n0 module_foo.mo:worker 1 0xa count=2 tick=10\n'


def boot(name, conf, expected, *, users=0, damaged=False, missing=False, raw=None, decoy=False, mode="base", test_config=None, test_user=True, allow_queued=False, forbidden=()):
    case = root/'cases'/name
    tree = case/'iso_root'
    shutil.rmtree(tree, ignore_errors=True)
    shutil.copytree(root/'iso_root', tree)
    archive = tree/'boot/initrd.cpio'
    if missing:
        archive.unlink()
    elif raw is not None:
        archive.write_bytes(raw)
    else:
        with archive.open('wb') as stream:
            if conf is not None:
                write_entry(stream, 'conf/task.conf', conf, mtime=0)
            write_entry(stream, 'task/user.elf', user, mtime=0)
            write_entry(stream, 'task/module_foo.mo', worker, mtime=0)
            if damaged:
                # Full ELF header, but a truncated program table. Runtime must reject safely.
                write_entry(stream, 'task/bad.elf', user[:64], mtime=0)
            write_trailer(stream, mtime=0)
    limine = tree/'limine.conf'
    if missing:
        limine.write_text(limine.read_text().replace('    module_path: boot():/boot/initrd.cpio\n', ''))
    if decoy:
        (tree/'task').mkdir(exist_ok=True)
        (tree/'task/absent.elf').write_bytes(user)
        with limine.open('a') as stream:
            stream.write('    module_path: boot():/task/absent.elf\n')
    limine.write_text(limine.read_text().replace('boot=base', 'boot='+mode))
    if test_config is not None:
        (tree/'conf').mkdir(exist_ok=True)
        config_name = 'task.conf' if mode == 'compat' else 'test.conf'
        (tree/'conf'/config_name).write_bytes(test_config)
        with limine.open('a') as stream:
            stream.write(f'    module_path: boot():/conf/{config_name}\n')
        if test_user:
            (tree/'task').mkdir(exist_ok=True)
            (tree/'task/user.elf').write_bytes(user.replace(b'LaOS user ELF: loaded OK', b'LaOS test ELF: loaded OK'))
            with limine.open('a') as stream:
                stream.write('    module_path: boot():/task/user.elf\n')
    iso = case/'LaOS.iso'
    with (case/'build.log').open('wb') as log:
        subprocess.run(['xorriso', '-as', 'mkisofs', '-b', 'limine-bios-cd.bin',
                        '-no-emul-boot', '-boot-load-size', '4', '-boot-info-table',
                        '--efi-boot', 'limine-uefi-cd.bin', '-efi-boot-part',
                        '--efi-boot-image', '--protective-msdos-label',
                        str(tree), '-o', str(iso)], stdout=log, stderr=log, check=True)
        subprocess.run(['third_party/limine-c-template/limine-binary/limine', 'bios-install', str(iso)],
                       stdout=log, stderr=log, check=True)
    serial = case/'serial.log'
    serial.unlink(missing_ok=True)
    with (case/'qemu.log').open('wb') as log:
        process = subprocess.Popen([os.environ.get('QEMU_SYSTEM_X86_64', 'qemu-system-x86_64'),
            '-accel', os.environ.get('X86_64_QEMU_ACCEL', 'tcg'), '-machine', 'q35', '-cdrom', str(iso),
            '-m', '512M', '-smp', '2', '-net', 'none', '-no-reboot', '-no-shutdown',
            '-display', 'none', '-monitor', 'none', '-serial', f'file:{serial}'], stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + int(os.environ.get('X86_64_QEMU_TIMEOUT', '60'))
            while time.monotonic() < deadline:
                text = serial.read_text(errors='replace') if serial.exists() else ''
                if all(marker in text for marker in expected):
                    assert not any(marker in text for marker in forbidden), text
                    if users:
                        roots = re.findall(r'\[task\] private user root \w+=(0x[0-9a-f]+)', text)
                        exits = re.findall(r'\[user-exit\] \w+ code=0', text)
                        if len(exits) < users:
                            time.sleep(.1)
                            continue
                        assert len(roots) == users and len(set(roots)) == users, text
                        assert text.count('M3c: ELF -> EL0 -> syscall write -> exit, DONE') == users, text
                        assert '[PANIC ' not in text and 'KERNEL EXCEPTION' not in text, text
                        subprocess.run([sys.executable, 'script/check_boot_phases.py', str(serial)], check=True)
                    else:
                        assert '[PANIC ' in text and (allow_queued or '[task] queued' not in text), text
                    print(f'PASS: initrd {name}', flush=True)
                    return
                if process.poll() is not None:
                    break
                time.sleep(.1)
            raise RuntimeError(f'{name}: missing expected markers; {serial}')
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

if __name__ == "__main__":
    boot('multiuser', base + b'0 user.elf:second 3 0xc 2 10\n0 user.elf:third 3 0xc 3 10\n',
         ['[task] queued third', '[module-foo] resumed after timeout'], users=3)
    boot('bad-user-then-valid', base.replace(b'0 user.elf:init', b'0 bad.elf:bad 3 0xc\n0 user.elf:init'),
         ['[task] failed to launch bad from bad.elf (type=3)', '[module-foo] resumed after timeout'],
         users=1, damaged=True)
    boot('missing-archive', None, ['base initrd missing or ambiguous'], missing=True)
    boot('malformed-archive', None, ['base initrd configuration failed'], raw=b'not a cpio')
    boot('missing-config', None, ['base initrd configuration failed'])
    boot('test-directive', base + b'@test registry\n', ['base initrd configuration failed'])
    boot('no-source-fallback', base.replace(b'user.elf', b'absent.elf'),
         ['invalid base task or missing component: absent.elf'], decoy=True)
