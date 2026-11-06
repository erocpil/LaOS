#!/usr/bin/env python3
"""A missing runtime payload must never pass through a built-in fallback."""
import sys
sys.dont_write_bytecode = True
from test_x86_64_initrd_cases import boot, base
names = ('priority', 'remote_enqueue', 'rcu_publish', 'rcu_stress')
config = b'@version 1\n' + ''.join(
    f'@test {name} module=test_{name}.mo\n' for name in names).encode()
expected = ['[user-exit] init code=0']
for name in names:
    expected += [f"payload 'test_{name}.mo' not found in limine-tests",
                 f"[selftest] WARNING: '{name}' not registered"]
boot('runtime-payloads-no-fallback', base, expected, users=1, mode='test',
     test_config=config, test_user=False,
     forbidden=[f"[selftest] '{name}' PASSED" for name in names])
