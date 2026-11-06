#!/usr/bin/env python3
"""Check BSP startup ordering and separation of unit fixtures from devices."""
import argparse
from pathlib import Path
import re
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('serial_log', type=Path)
parser.add_argument('--expect-bad-lafs', action='store_true')
args = parser.parse_args()
text = args.serial_log.read_text(errors='replace')
phases = ['memory', 'bootstrap-tests', 'devices', 'runtime', 'smp', 'services']
matches = list(re.finditer(r'\[boot\] ([a-z-]+) complete', text))
errors = []
if [m.group(1) for m in matches] != phases:
    errors.append('missing, duplicate, or out-of-order boot phase markers')
else:
    unit_end = matches[1].start()
    for marker in ('[test_vma] PASSED', '[test_cpio] PASSED',
                   '[test_lfs] PASSED', '[test_block_device] PASSED'):
        if text.count(marker) != 1 or marker not in text[:unit_end]:
            errors.append('unit fixture must run once before devices: ' + marker)
    if '[test_user_loader]' in text:
        marker = '[test_user_loader] PASSED'
        if text.count(marker) != 1 or marker not in text[:unit_end]:
            errors.append('user loader ownership check must pass once before devices')
    if args.expect_bad_lafs:
        device_log = text[matches[1].end():matches[2].start()]
        if '[lafs] bad magic' not in device_log or '[lafs] mounted:' in device_log:
            errors.append('real bad-image probe must reject the mount in the device phase')
if '[PANIC ' in text or 'KERNEL EXCEPTION' in text:
    errors.append('fatal kernel diagnostic in startup log')
if errors:
    for error in errors:
        print('  FAIL: ' + error, file=sys.stderr)
    sys.exit(1)
print('  PASS: boot phases and isolated bootstrap fixtures')
