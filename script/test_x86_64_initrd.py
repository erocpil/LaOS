#!/usr/bin/env python3
"""Bounded external initrd boot: sources, private users, queueing and execution."""
import os
from pathlib import Path
import re
import subprocess
import sys
import time

iso, directory = sys.argv[1:]
root = Path(directory)
serial, qlog = root/'initrd-serial.log', root/'initrd-qemu.log'
serial.unlink(missing_ok=True)
command = [os.environ.get('QEMU_SYSTEM_X86_64', 'qemu-system-x86_64'),
           '-accel', os.environ.get('X86_64_QEMU_ACCEL', 'tcg'), '-machine', 'q35',
           '-cdrom', iso, '-m', '512M', '-smp', '2', '-net', 'none',
           '-no-reboot', '-no-shutdown', '-display', 'none', '-monitor', 'none',
           '-serial', f'file:{serial}']
markers = ['[test_user_loader] PASSED', '[boot-config] purpose=0 selected base-initrd:/conf/task.conf',
           '[boot-config] base initrd ready', '[task] private user root init=',
           '[task] queued init from user.elf (type=3)',
           '[task] queued worker from module_foo.mo (type=1)',
           '[module-foo] resumed after timeout',
           'M3c: ELF -> EL0 -> syscall write -> exit, DONE',
           '[user-exit] init code=0']
with qlog.open('wb') as log:
    process = subprocess.Popen(command, stdout=log, stderr=log)
    try:
        deadline = time.monotonic() + int(os.environ.get('X86_64_QEMU_TIMEOUT', '60'))
        text = ''
        while time.monotonic() < deadline:
            text = serial.read_text(errors='replace') if serial.exists() else ''
            if '[PANIC ' in text or 'KERNEL EXCEPTION' in text:
                raise SystemExit(f'FAIL: fatal diagnostic; {serial}')
            if all(marker in text for marker in markers):
                if re.search(r"\[selftest\].*(?:PASSED|FAILED|registered 'registry')", text) or 'boot-modules-compat' in text:
                    raise SystemExit('FAIL: base boot consumed compatibility tests')
                subprocess.run([sys.executable, 'script/check_boot_phases.py', str(serial)], check=True)
                print('PASS: external initrd sources, private user root, queueing, worker progress and user exit')
                break
            if process.poll() is not None:
                raise SystemExit(f'FAIL: QEMU exited; {qlog}')
            time.sleep(0.1)
        else:
            missing = [m for m in markers if m not in text]
            raise SystemExit(f'FAIL: missing {missing}; {serial}')
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
