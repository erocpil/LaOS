#!/usr/bin/env python3
"""Mode and source isolation, using real QEMU boot and separately marked ELFs."""
import sys
sys.dont_write_bytecode = True
from test_x86_64_initrd_cases import boot, base

config = b'@version 1\n@module_missing panic\n@test registry\n0 user.elf:probe 3 0xc\n'
boot('explicit-test-mode', base,
     ['purpose=0 selected base-initrd:/conf/task.conf',
      'purpose=1 selected limine-tests:/conf/test.conf',
      '[selftest] \'registry\' PASSED',
      'user.elf resolved from limine-tests:/task/user.elf',
      'LaOS user ELF: loaded OK', 'LaOS test ELF: loaded OK',
      '[user-exit] init code=0', '[user-exit] probe code=0'],
     users=2, mode='test', test_config=config)
boot('base-ignores-tests', base,
     ['[user-exit] init code=0', '[module-foo] resumed after timeout'],
     users=1, test_config=b'not a valid configuration')
boot('test-config-no-fallback', base, ['test boot configuration failed'], mode='test')
boot('test-component-no-fallback', base, ['missing module user.elf'], mode='test',
     test_config=config, test_user=False, allow_queued=True)
boot('test-mode-requires-base', None, ['base initrd missing or ambiguous'], mode='test',
     test_config=config, missing=True)

boot('explicit-compat-mode', None,
     ['purpose=3 selected boot-modules-compat:/conf/task.conf',
      'LaOS test ELF: loaded OK', '[user-exit] legacy code=0'],
     users=1, mode='compat', missing=True,
     test_config=b'@version 1\n@module_missing panic\n0 user.elf:legacy 3 0xc\n')
