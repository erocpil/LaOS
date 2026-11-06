# PCI tutorial

[中文版](pci-tutor-zh.md)

PCI discovery answers which device is present and where its control registers
are located. Driver initialization then determines how to use that device.
Read the [architecture note](pci-arch.md) alongside this tutorial.

## 1. Decode a BDF

For bus 0, device 3, function 0, the ECAM offset of register 0x10 is:

```text
(0 << 20) + (3 << 15) + (0 << 12) + 0x10 = 0x18010
```

Add this to the mapped ECAM base, not to the device's BAR. ECAM contains
configuration registers; BARs describe other resources such as the NIC's
MMIO register bank.

## 2. Read the enumeration loop

In `kernel/pci.c`, follow `pci_scan_all_buses()`. Find the absent-device test,
the multifunction test, BAR parsing and insertion into `g_pci_devices`.
Then find the e1000 lookup for vendor/device `8086:100e`.

Explain why a device can appear in the list while its driver still returns
an initialization error: discovery does not allocate descriptor rings, enable
DMA or provide a usable interrupt route.

For each discovered device, keep a five-part record: BDF, class code, parsed
BAR address/flags, command-register bits and selected interrupt route. Do not
replace this record with a single “PCI device found” message.

## 3. Distinguish three addresses

For one device, identify its configuration-space address, its BAR physical
address and the kernel virtual mapping used to access that BAR. For a DMA
buffer, add a fourth value: the physical address written into a descriptor.
These values may be related by mappings, but they are not interchangeable.

## 4. Exercise a real PCI transport

```sh
make test-x86_64-lafs
```

Inspect PCI discovery followed by virtio queue setup and real LaFS mount/read
markers. The in-memory LaFS unit fixture can pass without any PCI disk; the
integration target is useful because it requires the real transport path.

## 5. Check interrupts separately

A capability-list entry says that the device offers MSI. It does not say
that LaOS has enabled it or installed a working handler. For x86_64 e1000,
follow `pci_enable_intx()` and the callback registration instead.

Continue with the [e1000 tutorial](e1000-tutor.md). If discovery works but
traffic does not, retain the BDF, BAR, command-register state and selected
IRQ route as separate pieces of evidence.

On ARM64, repeat the exercise through the platform ECAM path and distinguish
MSI selection from INTx fallback. On x86_64, follow the loadable e1000 module's
INTx registration separately from PCI capability discovery.
