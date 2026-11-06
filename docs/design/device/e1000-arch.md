# e1000 architecture

[中文版](e1000-arch-zh.md)

## Scope

The x86_64 e1000 driver is loaded from `e1000.mo`; ARM64 also contains an
integrated e1000 path used by the direct and Limine boot flows. Both paths
demonstrate PCI MMIO, DMA rings, packet processing and interrupt notification.
Packet helpers do not provide sockets, TCP connections or a general TCP/IP
stack, and the driver is not a general multi-NIC binding framework.

| File | Responsibility |
| --- | --- |
| `module/e1000.c` | initialization, RX/TX paths, worker modes and diagnostics |
| `module/e1000.h` | registers, descriptors, ring and packet types |
| `module/protocol.c`, `module/protocol.h` | teaching packet helpers |
| `kernel/pci.c` | discovery and configuration access |
| `kernel/arch/x86_64/idt.c`, `kernel/arch/aarch64/idt.c` | dispatch the e1000 IRQ callback |
| `conf/task.conf` | normal module placement and parameters |

## Initialization

`e1000_init()` finds the device, obtains and maps BAR0, enables PCI decoding
and bus mastering, resets the controller, reads the MAC address and prepares
RX/TX rings. Descriptor addresses presented to the device are physical;
the CPU accesses their storage through mapped virtual addresses.

The x86_64 module registers `e1000_irq_handler()` with the IDT layer after
driver state exists, clears stale causes and enables receive causes on the
INTx path. Capability discovery alone does not activate MSI on this path.
The ARM64 path tries MSI first and falls back to INTx when no usable MSI route
is available.

Initialization has an all-or-nothing boundary:

```text
PCI match -> BAR0 map -> bus mastering -> reset/MAC read
          -> RX ring and buffers -> TX ring and buffers
          -> IRQ callback registration -> interrupt causes enabled
```

An allocation, mapping or device-discovery failure must stop before exposing a
partially initialized ring to the device. The current exit helper disables RX
and unregisters the callback, but it is not a complete hot-unplug or module
unload protocol; workers, DMA activity and all outstanding buffers require a
coordinated shutdown.

The register contract is intentionally small. BAR0 is accessed through the
driver's MMIO helpers; `RCTL`/`TCTL` enable receive/transmit engines,
`RDH`/`RDT` and `TDH`/`TDT` delimit ring ownership, `ICR` reports and clears
causes, and `IMS` controls notification causes. Writing a tail register is an
ownership publication, not merely a progress counter update.

## Descriptor ownership

RX and TX each use 256 descriptors. RX descriptors point to supplied receive
buffers; TX uses per-slot buffers and serializes submission with `tx_lock`.
Driver and device exchange ownership through descriptor status and ring
head/tail state.

```text
RX: available buffer -> device writes packet -> DD observed
    -> driver consumes packet -> buffer/descriptor returned to device

TX: driver fills buffer/descriptor -> advances tail -> device sends
    -> DD observed -> slot can be reused
```

The TX path bounds packet length and waits for completion with a timeout.
A timeout must not be interpreted as successful transmission. Likewise,
an RX buffer cannot be freed or reused while a descriptor or software queue
still grants access to it.

The ownership transitions are the safety boundary:

```text
RX: device-owned descriptor
      -- DD observed --> CPU-owned packet/buffer
      -- release or replacement --> device-owned descriptor

TX: CPU-owned free slot
      -- descriptor + tail published --> device-owned slot
      -- DD observed --> CPU-owned reusable slot
```

The CPU must read an RX descriptor's completion status before consuming its
length and packet data. It must not write an RX tail or reuse the old buffer
until software consumers have finished. TX keeps the descriptor and backing
buffer reserved under `tx_lock` until completion or timeout handling finishes.

## Receive modes

`g_rx_mode` chooses one of five demonstration loops: simple polling, batch,
batch with buffer replacement, a single-thread pipeline, or a multithread
pipeline. The default is 5. `g_idle_mode` selects how polling loops yield or
sleep between attempts.

Batch release returns consumed descriptors to the ring. Buffer replacement
instead supplies a fresh device buffer and transfers the completed buffer
to software. The multithread path adds a queue and replenishment pool; its
ownership includes both the hardware ring and software consumers.

Changing receive mode changes these ownership transitions. It is not merely
changing the maximum number of packets processed per loop.

The modes have different lifetime shapes:

| Mode | Software ownership after DD | Descriptor returned by |
| --- | --- | --- |
| simple polling | caller/packet handler temporarily owns the buffer | polling loop after handling |
| batch | batch array retains packet references | `e1000_rx_batch_release()` |
| batch-swap | batch owns the completed buffer; ring gets a replacement | replacement path immediately, packet release later |
| single-thread pipeline | software queue owns the completed buffer | queue consumer |
| multi-thread pipeline | queue and free pool split packet/buffer ownership | consumer plus `e1000_rx_release_buffer()` |

The batch-swap and pipeline modes therefore cannot use the simple mode's
“return descriptor after processing” assumption.

## IRQ and worker boundary

The IRQ callback reads ICR, records the cause, publishes a receive-pending
flag and can mark a registered blocked worker READY. It does not drain the
RX ring. Descriptor consumption remains in the receive loops.

`e1000_wait_rx_event()` provides a pending-event wait helper. Its existence
does not mean every receive mode uses an interrupt-driven wait. Trace the
selected loop's call sites before making that claim.

The callback targets one global driver instance. Interrupt routing, worker
affinity and notification are not a general multi-NIC event subsystem.

The notification sequence is:

```text
device raises IRQ -> handler reads ICR and records cause
                  -> release-publishes rx_pending
                  -> optional blocked worker becomes READY
                  -> worker/poll loop consumes descriptors
                  -> worker clears pending state before waiting again
```

The handler is intentionally bounded and does not parse packets. An IRQ count
proves that a cause was delivered, not that a descriptor was completed,
replenished, queued, consumed or released.

## DMA ordering and lifetime

The driver uses `arch_dma_sync_for_cpu()` and
`arch_dma_sync_for_device()` around descriptor/buffer ownership changes.
Descriptor fields visible to hardware are volatile, but volatile alone does
not provide cache maintenance or inter-CPU publication ordering.

QEMU/x86_64 coherent DMA behavior cannot validate cache maintenance on another
architecture. The module's exit helper also does not establish safe general
unload: hardware, callbacks and worker references all need a coordinated
shutdown before code or buffers can be reclaimed.

## Architecture boundary

| Concern | x86_64 | ARM64 |
| --- | --- | --- |
| integration | loadable `e1000.mo` module | built-in path in the Limine/direct kernel flow |
| interrupt dispatch | loadable-module IDT callback using the cached PCI IRQ line | built-in callback path; MSI is preferred and INTx is the fallback |
| normal smoke coverage | `-net none`, no data-plane proof | networked direct/LaFS experiments, not the normal Limine smoke proof |
| DMA ordering evidence | coherent QEMU behaviour plus source audit | source audit and platform DMA hooks; requires runtime traffic for data-plane evidence |

The common driver code is not evidence that both architecture paths have the
same boot, interrupt or network coverage. Validate the selected boot path and
record device discovery, IRQ delivery, descriptor progress and packet handling
separately.

## Evidence

The standard x86_64 smoke target disables networking. For device and IRQ
evidence, follow the [e1000 tutorial](e1000-tutor.md) with an attached NIC.
Record discovery, initialization, cause delivery and packet progress
separately. An increasing IRQ count is not proof of successful RX ownership
transfer or protocol processing.

The minimum evidence set is:

| Evidence | Checks |
| --- | --- |
| PCI/BAR log | device match, BAR mapping and bus mastering |
| initialization log | reset, MAC read, ring allocation and enabled causes |
| `[e1000] IRQ` log | cause delivery and bounded handler progress |
| RX/TX counters | descriptor completion and software progress |
| packet or ping trace | end-to-end buffer ownership and protocol helper progress |

No single row is sufficient to prove the whole path.
