# TTY and monitor architecture

## Scope

The current subsystem is an x86_64-oriented framebuffer multiplexer plus one
monitor thread.  Its stable internal responsibilities are:

- filter framebuffer writes by active page and thread ownership;
- retain a bounded coloured character grid per page;
- serialize framebuffer rendering;
- switch and redraw pages;
- periodically render selected kernel metrics;
- keep serial output independent from page selection.

It is not a character-device subsystem or user-process terminal interface.

## Components

| Component | Responsibility |
| --- | --- |
| `kernel/tty.c` | page records, active ID, visibility, cell retention, redraw |
| `kernel/printf.c` | formatting, global framebuffer cursor, pixel drawing, serial output and `print_lock` |
| `kernel/monitor.c` | monitor-thread lifecycle, page selection and refresh cadence |
| `kernel/stats.c` | collect/render system, config, network, RCU and CPU views |
| `kernel/serial_input.h` | minimal ESC-digit page switching |
| `kernel/arch/x86_64/idt.c` | PS/2 and COM1 byte producers |
| `kernel/tty_test.c` | visibility-policy test |

## Static state

There are ten `struct tty` objects.  Each contains:

- an ID field, currently not initialised or consulted;
- framebuffer-derived row/column dimensions;
- an 80×25 cell grid;
- per-TTY cursor fields, currently unused;
- a current-process field, currently unused;
- a 64-byte input queue with head/tail, currently unused;
- a per-TTY spinlock, currently unused.

Active-page state is the atomic `current_tty_id`, initialised to `-1` and set to
zero by `tty_init()`.

The actual rendering cursor is the file-static `cursor_x/cursor_y` pair in
`kernel/printf.c`.  This hidden shared state is why page switching does not
provide independent terminal cursors.

## Output transaction

`kprintf()` and `kprintf_color()`:

1. disable local interrupts and acquire `print_lock`;
2. format into the single global `KPRINTF_BUF_SZ` buffer;
3. mark truncated output if necessary;
4. render to framebuffer when boot is early or `tty_ready()` permits it;
5. always write the formatted bytes to serial;
6. release the lock and restore interrupt state.

The lock protects:

- the global formatting buffer;
- framebuffer cursor movement;
- framebuffer pixels;
- the active TTY grid writes caused by drawing.

This makes a formatted call atomic with respect to other formatted calls, but
long monitor frames consist of many calls.  Monitor code disables preemption
to avoid local visual interleaving; remote CPUs can still print between calls
when visibility policy permits.

## Visibility policy

After the TTY is initialised:

```text
active == 0                  -> framebuffer allowed
active in 1..9 and
current_thread.tty_id bit    -> framebuffer allowed
otherwise                   -> serial only
```

The code describes TTYs 6–9 as dedicated monitor channels, but the generic bit
check applies to any nonzero active ID.  Current callers only expose the
documented pages.

`tty_ready()` assumes a valid current CPU/thread when SMP has reached the
filtered-output phase.  Boot-time printing bypasses it while `online` differs
from the final CPU count.

Thread `tty_id` is a bitmask despite its singular name.  The monitor thread
sets bits 0, 6, 7, 8 and 9.  Ordinary threads start with zero; TTY 0 remains a
wildcard.

## Switch transaction

`tty_switch(new_id)` validates the range and ignores a no-op switch.  Under
`print_lock` it:

```text
current_tty_id = new_id
clear all framebuffer pixels
for each cell in destination grid:
    draw nonempty cells
```

`draw_char()` records the redrawn cell again into the now-active grid.  This is
idempotent for the current data.

Switching is O(framebuffer pixels + rows×columns).  It executes with local
interrupts disabled and is invoked from input interrupt paths, so a large
framebuffer can create substantial interrupt latency.

The switch does not:

- save/restore a per-page cursor;
- snapshot output occurring on inactive pages;
- notify producers;
- synchronize with an unused per-TTY lock;
- scroll or resize existing contents.

## Framebuffer bounds

The retained grid is capped at 80×25, while pixel clearing covers the full
framebuffer.  `tty_record_char()` ignores cells beyond the retained dimensions.
If the framebuffer is larger, output can be visible in pixels that will not be
restored after a switch.

The character renderer assumes an 8×16 font and a 32-bit framebuffer accessed
as `pitch / 4`.  It does not negotiate arbitrary pixel formats.

The printer wraps vertically to the top without clearing.  Retained cells are
overwritten in place, giving dashboard-friendly bounded storage rather than
terminal scrolling.

## Monitor scheduling

`start_monitor()` creates a thread targeting `g_cpu_count - 1`, names it
`monitor`, and enqueues it.  The function does not currently handle thread
creation failure before dereferencing the result.

`monitor_ready` is set by the thread itself, not by its creator.  Input paths
use this flag to prevent selection before the renderer has actually run.
The flag is a plain `bool`; its current use assumes simple coherent visibility
and does not define a formal release/acquire handshake.

The monitor loop uses:

- a shared last-render tick for dynamic pages;
- a last-page ID for entry-only pages;
- `schedule_timeout(1)` while visible;
- longer sleeps for static pages.

Only the active page is rendered, avoiding unnecessary framebuffer work.

## Page data contracts

### TTY 9: CPU

Uses a single TSC sample for a frame and each CPU's runqueue lock to align
`current`, runtime accumulation and prior snapshots.  The cross-CPU TSC
assumption is platform-specific.

### TTY 8: RCU

Traverses the RCU test metrics list and displays reader counters and per-CPU
generation progress.  The metrics are test instrumentation and do not have a
complete snapshot lock.

### TTY 7: network

Reads an `e1000_stats` pointer registered by a module.  It copies a previous
frame to derive rates.  The pointer and fields have no lifecycle/refcount
contract suitable for module unload.

### TTY 6: configuration

Traverses the parsed directive and task lists.  It assumes boot configuration
is immutable after monitor startup.

### TTY 0: system

Appends a system summary on page entry without clearing prior boot output.
It is an overview, not a continuously refreshed page.

## Input boundary

PS/2 input decoding is architecture-specific in
`kernel/arch/x86_64/idt.c`.  It tracks Shift, Control, Alt and Caps Lock and
maps set-1 scancodes to ASCII.

Current delivery endpoints:

- Alt-number selects a page;
- plain `c` clears the framebuffer;
- COM1 bytes call the shared serial helper;
- other translated characters have no queue/consumer;
- Control combinations are discarded after translation.

The input queue fields in `struct tty` are not connected.  There is no wait
queue, blocking read, canonical buffering, echo policy, foreground process
group or user-copy boundary.

## Platform boundary

The common files compile on both branches, but the maintained graphical path
is wired by x86_64 Limine framebuffer initialisation and x86 interrupt input.
ARM64 direct boot is serial-oriented, and the ARM64 boot code does not start
the same TTY monitor.

Cross-architecture documentation must therefore distinguish shared source from
runtime feature parity.

## Validation and gaps

Existing evidence:

| Evidence | Covers |
| --- | --- |
| `kernel/tty_test.c` with `CONFIG_TTY_TEST=1` | visibility decisions |
| normal x86_64 boot | initialisation and monitor thread creation |
| manual Alt/ESC switching | page selection and visible redraw |
| serial QEMU logs | output remains available independent of framebuffer |

Missing automated evidence:

- the TTY test is disabled by default and not gated by a Make target;
- no redraw/cursor/scroll test;
- no concurrent switch/print stress;
- no framebuffer pixel-format test;
- no input queue or line-discipline tests;
- no ARM64 monitor runtime gate.

## Evolution order

1. Rename or document `tty_id` explicitly as a page mask.
2. move cursor state into `struct tty`;
3. add a bounded ring-buffer input API and wait queue;
4. move page switching out of hard IRQ context if full-frame clears remain;
5. add scroll/resize behaviour and framebuffer-format validation;
6. separate a `dashboard` abstraction from future process-facing terminal
   devices;
7. add deterministic rendering and input integration tests.
