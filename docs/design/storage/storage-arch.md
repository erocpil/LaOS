# Storage subsystem architecture

## Scope

The current storage subsystem demonstrates a complete read path on x86_64 and
ARM64:

```text
LaFS -> block_device -> architecture virtio-blk transport -> QEMU
```

Its intended use is boot-time examples and deterministic selftests.  It is not
yet a general-purpose block layer or filesystem service.

## Component ownership

| Component | Responsibility | Must not own |
| --- | --- | --- |
| `kernel/block_device.c` | device registration and generic sector-read dispatch | PCI/MMIO discovery, filesystem policy |
| `kernel/lafs.c` | LaFS mount, lookup, read and directory parsing | transport details, DMA |
| `kernel/arch/x86_64/virtio_pci.c` | PCI discovery, modern virtio negotiation and queue operation | LaFS format |
| `kernel/arch/aarch64/virtio_blk.c` | virtio-MMIO negotiation and queue operation | LaFS format |
| `script/mkfs_lafs.py` | deterministic version-1 test image construction | runtime mount policy |

Shared filesystem and block-device changes belong on `x86_64` first.  Transport
changes remain architecture-specific unless they change a shared contract.

## Generic block-device contract

The contract in `kernel/block_device.h` is intentionally small:

```c
int read(struct block_device *dev,
         uint64_t sector,
         void *buffer,
         size_t count);
```

Current invariants:

- `sector_size` describes the registered device;
- a successful callback returns zero;
- the caller supplies storage for `count` sectors;
- registration stores the device pointer; it does not copy the object;
- the registry holds at most eight devices;
- `block_read()` dispatches to the last registered device.

Consequences:

- registered device objects must outlive their registration;
- there is no stable device namespace;
- concurrent registration/read/reset is unsupported;
- partitioning, request merging and asynchronous completion are outside this
  layer.

The last-registered selection rule is test-friendly but architecturally weak.
Any multi-disk work must replace it with an explicit device handle or lookup.

## Virtio request model

Both implementations expose synchronous one-sector reads to the generic layer
and use a three-descriptor virtqueue chain.  They differ at the transport and
DMA details.

| Property | x86_64 | ARM64 |
| --- | --- | --- |
| Transport | modern virtio PCI capabilities | virtio MMIO v1/v2 |
| QEMU device in LaFS test | `virtio-blk-pci` | `virtio-blk-device` |
| Completion | polling | polling |
| Data placement | page-local bounce buffer | caller buffer |
| Ordering | compiler barriers/coherent DMA assumption | explicit `dmb ishst` publication barrier |
| Interrupt use | none | none |

The queue has one producer in current boot/test use.  There is no request lock,
request-ID allocator, scheduler, cancellation, recovery or interrupt-driven
completion.

### Required publication order

For each request:

1. fill the request header and descriptor chain;
2. put the descriptor head in the available ring;
3. publish the available index with the architecture-required ordering;
4. notify the device;
5. wait for a used-ring entry;
6. validate the device status before returning data.

Future optimisation must preserve this order.  A compiler barrier alone is not
a portable DMA-ordering abstraction across architectures.

## LaFS format and runtime model

LaFS is a fixed-layout read-only filesystem:

- 512-byte sectors;
- a superblock at sector zero;
- 128-byte fixed inodes;
- inode types free/file/directory;
- twenty direct data sectors per inode;
- variable-length directory entries;
- no allocation metadata required at runtime.

The runtime implementation has one global mount state and shared sector
buffers.  It is consequently:

- non-reentrant;
- not safe for concurrent readers without external serialisation;
- unable to represent multiple mounted filesystems;
- unable to retain an open-file object independent of the global mount.

The root is currently opened as inode zero.  Although the image contains a
root-inode field, the runtime does not yet use it as the authoritative root.

## Error model

At present, errors are represented by integer return codes but are not
systematically classified.  The code detects important failures such as
missing devices, bad magic, unknown paths and device-reported read failure.

Missing parts of the contract include:

- supported format-version negotiation;
- overflow-safe superblock and inode bounds validation;
- validation that direct block numbers belong to the image/data area;
- distinct timeout, malformed-image and transport errors;
- recovery or device reset after a stuck request;
- a consistent timeout policy on both architecture drivers.

A malformed image should eventually fail mount or lookup without permitting an
out-of-image read.  That is a stronger requirement than the current trusted
fixture provides.

## Concurrency and lifetime

The current safe operating assumptions are:

- storage initialisation and registration are single-threaded;
- one filesystem operation is active at a time;
- a registered driver object and its queue memory live for the rest of boot;
- the backing image is immutable;
- no device is removed.

If tasks begin using LaFS concurrently, the first necessary change is not a
mutex around every global.  The better boundary is a mount object containing
device identity and filesystem state, plus per-operation or caller-owned
buffers.  Locks can then protect defined objects instead of preserving hidden
global coupling.

## Validation matrix

| Layer | Evidence | Covered behaviour |
| --- | --- | --- |
| block registry | `kernel/test_block_device.c` | registration, dispatch, multiple devices, capacity |
| LaFS parser | `kernel/test_lafs.c` | bad magic, mount, lookup, content, directory iteration |
| x86_64 integration | `make test-x86_64-lafs` | PCI virtio initialisation and real image reads |
| ARM64 integration | `make test-arm64-lafs` | MMIO virtio direct-boot path and real image reads |
| ARM64 failure path | `make test-arm64-lafs-negative` | invalid image does not produce successful mount |

Passing the memory-backed tests does not prove descriptor publication, DMA
addressing or transport discovery.  Passing only a QEMU smoke boot does not
prove malformed-format handling.  Both levels are required.

## Evolution boundary

Near-term improvements that preserve the teaching scope:

1. validate version, ranges, arithmetic and root inode at mount;
2. align timeout/status behaviour across both virtio drivers;
3. make device selection explicit;
4. replace global LaFS state with an explicit mount object;
5. document and test the DMA-address contract.

Features such as writeback caching, journaling, indirect extents, hotplug and
high-throughput multi-queue I/O should be added only when they serve a stated
learning objective.  Adding them without first fixing identity, lifetime,
error and concurrency contracts would increase code volume more than
architectural value.
