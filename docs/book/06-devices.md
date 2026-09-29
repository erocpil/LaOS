# Chapter 6: Devices

[中文版](06-devices-zh.md)

> **Prerequisites**: [Memory](02-memory.md), [Interrupts](03-interrupt.md) and
> [Scheduling](04-scheduler.md).
> **You'll build**: a distinction between discovery, data transfer and notification.
> **Cross-reference**: [PCI tutorial](../design/device/pci-tutor.md),
> [e1000 tutorial](../design/device/e1000-tutor.md) and
> [storage tutorial](../design/storage/storage-tutor.md).

---

## 6.1 Discovery locates an interface

PCI ECAM provides configuration registers addressed by bus, device and
function. Enumeration reads IDs and resource metadata, then records devices
for drivers to find. A BAR identifies a resource such as an MMIO register
bank; it is distinct from the ECAM configuration address.

A device appearing in the list is the beginning of initialization. The driver
still needs decoding, bus mastering where appropriate, mapped registers and
valid data structures for the device to consume.

## 6.2 DMA adds another memory user

The e1000 device reads and writes descriptor rings and packet buffers without
executing the CPU's C code. A CPU-side lock coordinates software callers,
but hardware ownership also depends on descriptor status and head/tail state.

The driver must finish filling a buffer and descriptor before notifying the
device. On receive, it must observe completion before consuming packet bytes,
and return a usable buffer before making the slot available again.

Physical addresses in descriptors and virtual pointers in software are not
interchangeable. Cache maintenance and ordering are also separate from the
`volatile` accesses used for hardware-visible fields.

## 6.3 Interrupts notify; workers consume

The x86_64 e1000 IRQ callback records ICR causes and can notify a worker.
The selected receive loop consumes descriptors. Polling and worker modes
have different buffer-lifetime paths; an interrupt count alone cannot tell
whether packets were processed correctly.

e1000 is a loaded module on this path. Its callback must target the loaded
instance, and its lifetime must cover any hardware or worker references.
Module code existing in memory is not a safe-unload protocol.

## 6.4 Storage separates format from transport

```text
LaFS file read -> block_device read -> virtio PCI request -> QEMU disk
```

LaFS owns filesystem interpretation. The block registry dispatches sector
reads. The virtio transport owns descriptor submission and completion. This
allows the parser to be tested with a memory-backed fixture without a disk.

That useful separation also creates a testing trap: a parser success does
not prove real device I/O. The maintained LaFS integration target requires
the real virtio-blk LaFS mount marker after the boot-time mock registry is
reset. Boot also attempts to read `/etc/motd`, but the target does not assert
its contents.

Current LaFS is read-only, virtio-blk polls, and the block registry selects
the last registered device rather than exposing a general device namespace.

## Experiment: compare two evidence chains

```sh
make test-x86_64-lafs
```

Trace the real disk path and compare it with the earlier LaFS unit output.
List the extra components exercised only by the real path.

For a separate NIC discovery experiment, follow the
[e1000 setup](../design/device/e1000-tutor.md#2-select-a-qemu-setup). The default
boot gate disables networking, so do not reuse its pass as NIC evidence.

**Previous**: [User mode](05-user-mode.md).
**Next**: [Synchronization](07-synchronization.md).
