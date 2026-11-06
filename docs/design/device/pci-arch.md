# PCI architecture

[中文版](pci-arch-zh.md)

## Scope and source map

PCI code discovers functions and exposes configuration-space access to
drivers. The shared enumeration code serves x86_64's ACPI/MCFG path and the
ARM64 ECAM base supplied by the platform boot path. This is a small
QEMU-oriented implementation, not a general PCI resource manager.

| File | Responsibility |
| --- | --- |
| `kernel/arch/x86_64/main.c`, `kernel/arch/aarch64/init_common.c` | obtain ECAM information and order PCI initialization |
| `kernel/arch/x86_64/acpi.c` | ACPI table support |
| `kernel/pci.c`, `kernel/pci.h` | ECAM access, enumeration, BAR/capability metadata |
| `module/e1000.c` | e1000 device selection and initialization |
| `kernel/arch/x86_64/virtio_pci.c` | virtio PCI transport discovery |

## Configuration access

`pcie_init()` maps the x86_64 ECAM window at `PCIE_VIRT_BASE` using
cache-disabled/write-through page flags. Configuration addresses follow:

```text
ECAM virtual base + (bus << 20) + (device << 15) + (function << 12) + offset
```

`pci_read8/16/32()` and matching writes select the register width. The width
is part of the contract: a word-sized command update must not accidentally
write adjacent status bits with write-one-to-clear semantics.

The mapped ECAM window and fixed addressing assumptions do not implement
arbitrary multi-segment MCFG layouts. Generalizing the bus range also requires
checking how its start bus is reflected in address calculation.

Configuration access is a discovery operation, not a driver activation
operation. A safe activation sequence is:

```text
read vendor/device -> parse class and BARs -> choose driver
                   -> enable memory/IO and bus mastering
                   -> establish BAR mapping and DMA state
                   -> choose and enable an interrupt route
```

The shared helpers do not roll this sequence back as a transaction. A caller
that enables command bits or writes a BAR must own the failure cleanup for the
resources it subsequently allocates.

## Enumeration and metadata

`pci_scan_all_buses()` walks the configured bus range, 32 slots per bus and
up to eight functions. A vendor ID of `0xffff` skips an absent slot. Function
zero's multifunction bit determines whether the other functions are probed.

Device records retain BDF, IDs, class information, BAR information and
interrupt/capability metadata. They are published in `g_pci_devices` and
drivers can locate a device through `pci_find_device()`.

BAR parsing separates flags from addresses and accounts for 64-bit memory
BARs occupying two slots. A parsed BAR is a device resource address, not a
CPU pointer that can be dereferenced before mapping. Enumeration is not
proof that a driver has enabled memory decoding or bus mastering.

## Interrupts and driver activation

Legacy INTx routing and MSI capability discovery are represented separately.
`pci_enable_intx()` enables the recorded legacy route. Merely discovering an
MSI capability does not establish vector allocation, delivery or dispatch.
The current x86_64 e1000 path uses INTx.

e1000 remains a loadable module. Boot caches its vector, and the module
registers its callback with the IDT layer. virtio-blk instead initializes its
transport during boot and polls its queue. Neither path is evidence of a
complete dynamic bind/unbind framework, despite the presence of driver
types in the header.

## Validation and limits

The interrupt helpers have distinct contracts:

| Helper | Effect | Does not prove |
| --- | --- | --- |
| `pci_enable_intx()` | enables legacy command bits and records the route used by the architecture | that a device-specific handler is installed |
| `pci_enable_msi()` | programs the supported MSI capability and enables MSI in the command register | that the platform will deliver the vector to the intended handler |
| driver callback registration | connects a device cause to a kernel callback | that the device cause is enabled or that the callback drains the device |

The e1000 path currently uses INTx on x86_64 and tries MSI before falling back
to INTx on ARM64. This policy belongs to driver/architecture integration, not
to capability discovery alone.

```sh
make test-x86_64-lafs
```

This target requires real virtio-blk/LaFS evidence and exercises PCI transport
discovery. The ordinary boot gate uses `-net none`, so it cannot validate an
e1000 IRQ or receive path. Use the [e1000 tutorial](e1000-tutor.md) for that
separate experiment.

Hotplug, general BAR allocation across bridges, broad firmware compatibility
and arbitrary interrupt topology are outside current coverage. Follow the
[PCI tutorial](pci-tutor.md) to distinguish discovery from device operation.

The minimum PCI evidence should preserve the BDF, raw BAR values, parsed BAR
flags, command-register state and selected interrupt route. A device-list
entry without those follow-up facts proves enumeration only.
