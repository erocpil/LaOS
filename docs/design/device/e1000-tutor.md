# e1000 tutorial

[中文版](e1000-tutor-zh.md)

This tutorial follows a packet buffer across the CPU/device boundary. Start
with the [PCI tutorial](pci-tutor.md) and use the
[architecture note](e1000-arch.md) for ring and worker ownership.

## 1. Start with device discovery

The normal module configuration loads e1000 on CPU 2. Read its task record
in `conf/task.conf`, then follow `main()` and `e1000_init()` in
`module/e1000.c`. Without a matching PCI device, initialization cannot reach
ring setup.

`make test-x86_64` passes `-net none`. A passing smoke gate therefore says
nothing about e1000 receive, transmit or INTx delivery.

## 2. Select a QEMU setup

For an interactive discovery experiment without creating a host TAP interface:

```sh
make run HEADLESS=1 QEMU_NET='-netdev user,id=u1 -device e1000,netdev=u1,mac=52:54:00:12:34:56'
```

This attaches the NIC through QEMU's user networking backend. It is useful
for enumeration and initialization, but does not reproduce the project's
TAP-based traffic experiments or establish end-to-end network support.
In the headless serial console, Ctrl-a x exits QEMU.

For ARM64, distinguish the built-in initialization in the direct/Limine kernel
path from the x86_64 loadable-module path. Shared driver functions do not imply
that the two paths have identical IRQ routing or test coverage.

For the existing TAP setup and host cleanup commands, use
[Getting started](../../getting-started.md). That setup changes host networking;
it is needed only when selecting that experiment.

## 3. Trace one receive descriptor

Read `e1000_init_rx_ring()`. Identify the CPU pointer to the ring, the physical
base written to hardware, and the receive buffer for one slot. Follow the
same slot through a polling loop until DD is observed and the tail advances.

For batch mode, find `e1000_rx_batch_release()`. For buffer-swap mode, identify
the replacement buffer before the old one reaches a software consumer.
Explain why returning the descriptor and freeing the completed packet are
different operations in the latter case.

Write down the owner of each object at every step:

```text
descriptor -> device
completed buffer -> batch/queue consumer
replacement buffer -> device after DMA preparation
released buffer -> free pool or allocator
```

If a buffer appears in two columns at once, the proposed transition is unsafe.

## 4. Separate notification from progress

Look for the INTx initialization message and, with suitable incoming traffic,
the bounded `[e1000] IRQ` diagnostic messages. The handler reads ICR and
records a pending event; it does not parse the packet.

If interrupts arrive but processing stalls, inspect the selected `g_rx_mode`,
descriptor DD state, software queue and buffer pool. If no interrupt arrives,
check device attachment, INTx enablement, interrupt masks and traffic before
changing scheduler code.

Read `e1000_irq_handler()` and `e1000_wait_rx_event()` together. The handler
records a bounded notification; the worker still owns descriptor consumption.
An IRQ log is therefore an intermediate observation, not a packet-success
result.

## 5. Exercise: explain TX completion

Follow `e1000_send_packet()` from `tx_lock` acquisition through copying data,
descriptor publication, tail update and completion polling. Find the timeout
return and explain why a caller must not count that result as a sent packet.

The demonstration includes packet helpers, but successful raw-frame exchange
does not imply sockets or TCP. Continue with
[Chapter 6: Devices](../../book/06-devices.md) for the storage comparison.

## 6. Evidence checklist

For each experiment, record the following separately:

1. PCI match, BAR mapping and bus-master enablement;
2. reset, MAC read and ring allocation;
3. interrupt cause delivery and pending-worker wakeup;
4. DD transitions and descriptor-tail updates;
5. packet consumption, buffer release and TX completion.

`make test-x86_64` is not sufficient because it disables networking. Use the
attached-NIC experiment for data-plane evidence, and treat TX timeout as a
failure rather than as a successful send.
