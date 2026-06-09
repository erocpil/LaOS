# Storage path tutorial

This tutorial follows one sector from a file read down to a virtual block
device.  It describes the current teaching implementation, not a production
storage stack.

## What the storage example teaches

LaOS deliberately keeps the path short:

```text
application or selftest
        |
        v
LaFS pathname and inode lookup
        |
        v
generic block-device API
        |
        v
architecture transport
  x86_64: virtio PCI
  ARM64:  virtio MMIO
        |
        v
QEMU virtio-blk device
```

This separation is the important part of the example.  LaFS does not know
whether the disk was discovered through PCI or MMIO, and the virtio drivers do
not parse files or directories.

The implementation is intentionally narrower than the diagram might suggest:

- block reads are synchronous and polling;
- one request is completed before the next starts;
- LaFS is read-only;
- one global LaFS instance is mounted;
- the filesystem image is trusted and has a small fixed layout.

## 1. Build the teaching image

Run:

```sh
make lafs-image
```

The image builder is `script/mkfs_lafs.py`.  It creates a small deterministic
image containing `/etc/motd` and `/etc/version`.  Keeping the fixture
deterministic lets the boot tests verify the real filesystem and device path
without depending on host files.

The on-disk layout is:

```text
sector 0          superblock
inode area        fixed-size 128-byte inode records
data area         file data and encoded directory entries
```

All sectors are 512 bytes in the current implementation.

## 2. Register a block device

The generic interface is declared in `kernel/block_device.h`.  A driver fills
in a `struct block_device` with:

- a diagnostic name;
- a sector size;
- driver-private data;
- a sector-read callback.

It then calls `block_device_register()`.  LaFS reaches the active device through
`block_read()` rather than calling an architecture driver directly.

The registry is deliberately simple.  It is a fixed-size append-only array and
has no locking or removal operation.  `block_read()` selects the most recently
registered device.  That last rule is useful for selftests, because an
in-memory test disk can temporarily override a real device, but it is not a
general device-selection policy.

`block_device_reset()` exists for tests.  Normal boot code should not use it to
model hot removal.

## 3. Discover and initialise virtio-blk

The two architectures share the block API but use different transports.

### x86_64

`kernel/arch/x86_64/virtio_pci.c` discovers a virtio block device on PCI,
locates the modern virtio capabilities, negotiates `VIRTIO_F_VERSION_1`, and
sets up one virtqueue.

The queue, descriptors, request header, bounce buffer and status byte fit in a
single physically contiguous page.  A read creates the standard three-element
descriptor chain:

```text
request header  ->  512-byte data buffer  ->  status byte
device reads        device writes             device writes
```

The driver publishes the descriptor head in the available ring, notifies the
device, then polls the used ring.  On success it copies the bounce buffer into
the caller's buffer.

### ARM64

`kernel/arch/aarch64/virtio_blk.c` uses the virtio-MMIO transport exposed by
QEMU's `virt` machine.  It supports the legacy version-1 and modern version-2
MMIO layouts and registers the same generic block-device interface.

The ARM64 LaFS test uses the direct-kernel QEMU path with
`virtio-blk-device`.  This is distinct from the ARM64 Limine boot path and
should not be taken as evidence that every firmware/boot combination exercises
the same storage setup.

## 4. Mount LaFS

`lafs_mount()` in `kernel/lafs.c` reads sector zero through the generic
`block_read()` API.  It verifies the magic and records the inode-table and data
layout needed by later operations.

The superblock fields currently used by the implementation are:

| Byte offset | Size | Meaning |
| --- | ---: | --- |
| 0 | 5 | `LaFS` plus format version byte |
| 5 | 4 | inode count |
| 9 | 4 | first inode-table sector |
| 13 | 4 | first data sector |
| 17 | 4 | root inode number |

The current mount code validates the four-byte `LaFS` magic.  The image builder
writes version 1, but version compatibility and structural bounds checks are
not yet a complete mount contract.

## 5. Resolve and read a file

`lafs_open()` starts at inode zero and walks slash-separated path components.
For each directory component it scans the directory's encoded entries and
loads the referenced inode.

Each inode is 128 bytes:

| Byte offset | Size | Meaning |
| --- | ---: | --- |
| 0 | 1 | type: free, file or directory |
| 1 | 32 | inode name |
| 33 | 4 | file size |
| 37 | 80 | twenty direct sector numbers |

A directory entry contains a four-byte inode number, a one-byte name length,
and that many name bytes.  There is no terminating zero in the on-disk entry.

`lafs_read()` translates a file offset into one of the twenty direct sectors,
reads the sector into a shared buffer, and copies the requested bytes.  There
are no indirect blocks.  With 512-byte sectors, the representable direct file
payload is therefore at most 10 KiB.

`lafs_readdir()` uses the same directory encoding to return one name and inode
number at a time.

## 6. Run the evidence-producing tests

For x86_64:

```sh
make test-x86_64-lafs
```

This boots QEMU with `virtio-blk-pci`, checks that the real virtio path is
initialised, mounts the generated image, and verifies its known files.

For ARM64:

```sh
make test-arm64-lafs
make test-arm64-lafs-negative
```

The positive target checks the direct-boot virtio-MMIO path and file contents.
The negative target supplies a bad image and checks that failure is handled
instead of silently reporting a successful mount.

The in-kernel unit-style coverage lives in `kernel/test_block_device.c` and
`kernel/test_lafs.c`.  It uses a memory-backed disk to test registry behaviour,
bad magic, pathname lookup, file reads and directory iteration.  These tests
are useful fault-localisation tools; the QEMU targets are still needed to prove
the transport and DMA path.

## Exercises

Useful extensions, in increasing order of architectural impact:

1. Reject unsupported superblock versions and out-of-range inode/data sectors.
2. Return explicit timeout and device-status errors from both virtio drivers.
3. Replace the implicit "last registered device" rule with lookup by identity.
4. Give LaFS a mount object and caller-owned I/O buffers.
5. Add more than one outstanding virtqueue request.
6. Add allocation and writes only after defining crash and consistency rules.

The first four improve the teaching implementation without turning LaFS into a
large filesystem project.  The last two change its concurrency and persistence
model and deserve their own design work.
