#!/bin/bash
# git bisect run script — check ARM64 EL0 entry succeeds (QEMU 10.0)
set -e
cd /root/src/LaOS

# Build
make kernel ARCH=aarch64 2>&1 | tail -1

# Run QEMU with highmem-ecam=off for QEMU 10.0 compat
mkdir -p build/bisect
timeout 25 qemu-system-aarch64 \
  -machine virt,gic-version=3,highmem-ecam=off -cpu cortex-a57 \
  -kernel bin-aarch64/kernel \
  -m 512M -smp 1 \
  -no-reboot -no-shutdown \
  -display none -serial file:build/bisect/serial.log \
  2>/dev/null; true

# EL0 success: AT PAR=F=0 AND "M3c: ... DONE" appears
if grep -q "PAR=0x.*F=0" build/bisect/serial.log && grep -q "M3c:.*DONE" build/bisect/serial.log; then
    echo "=== GOOD: EL0 works ==="
    exit 0
else
    echo "=== BAD ==="
    grep -E "PAR=|IAbort|DONE|System Halted" build/bisect/serial.log
    exit 1
fi
