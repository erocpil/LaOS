# TTY and monitor tutorial

LaOS calls its framebuffer page multiplexer a TTY, but it is intentionally much
smaller than a Unix terminal subsystem.  It provides several retained display
pages and live teaching dashboards.  It does not currently provide line
discipline, terminal device files, sessions, job control or a shell.

## Output paths

Kernel text has two output destinations:

```text
                 +--------------------> serial console
kprintf ---------|
                 +-- TTY visibility --> framebuffer --> retained character grid
```

Serial output is unconditional.  It remains the authoritative debugging and
test-harness stream even when a different framebuffer page is visible.

Framebuffer output is filtered by `tty_ready()` after SMP is online:

- TTY 0 accepts output from any thread;
- TTYs 6–9 accept output only from a thread whose `tty_id` bitmask contains
  the active page;
- an uninitialised TTY suppresses framebuffer output.

This prevents a normal worker's log from overwriting a monitor dashboard.

## Initialisation

`tty_init()` in `kernel/tty.c` reads the Limine framebuffer size, converts it to
an 8×16 character grid and clamps it to 80 columns by 25 rows.  It creates ten
static TTY records and activates TTY 0.

The current x86_64 boot path calls `tty_init()` early so boot text is retained
in the TTY 0 grid.  The ARM64 paths do not currently start the same framebuffer
TTY/monitor experience; their serial console remains the useful interactive
diagnostic channel.

If no framebuffer exists, the grid dimensions are zero.  Kernel printing still
reaches serial, while framebuffer drawing becomes a no-op.

## Retained character grids

Each TTY owns a fixed array of `struct tty_cell` records:

```c
struct tty_cell {
    char c;
    uint32_t fg;
    uint32_t bg;
};
```

`draw_char()` in `kernel/printf.c` writes eight by sixteen pixels and then calls
`tty_record_char()` to save the character and colours at that cell.

Switching pages:

1. takes the shared framebuffer/print lock;
2. updates the active TTY ID;
3. clears framebuffer pixels without clearing the retained grid;
4. redraws every nonempty cell in the destination grid;
5. releases the lock.

Calling the ordinary `fb_clear_screen()` here would be wrong because it also
clears the active TTY grid.  `tty_switch()` deliberately performs a pixel-only
clear before restoring the destination.

## What “retained” means here

The implementation retains characters that were actually rendered while a TTY
was active.  It is not a complete independent output stream per page.

When a monitor page is not active:

- its renderer sleeps;
- other threads fail the framebuffer visibility check;
- their messages still go to serial;
- those messages are not appended to the inactive page's grid.

There is also one global framebuffer cursor in `kernel/printf.c`.  The
`cursor_x` and `cursor_y` fields inside each `struct tty` are not currently
used.  Switching pages restores cells but not an independent cursor position.

When output reaches the bottom, the framebuffer printer wraps to the top
instead of scrolling retained rows.  This is sufficient for dashboards that
clear and repaint, but it is not terminal scrollback.

## Monitor pages

`start_monitor()` creates one monitor thread on the last configured CPU.  When
that thread begins running, it sets `monitor_ready` and grants itself access to
TTYs 0, 6, 7, 8 and 9.

The pages are:

| TTY | Renderer | Purpose | Refresh |
| ---: | --- | --- | --- |
| 0 | `stats_sys()` | boot/system overview and loaded tasks | once on entry |
| 6 | `stats_conf()` | parsed `task.conf`, directives and task rows | once on entry |
| 7 | `stats_net()` | e1000 mode, link, traffic, queues and hardware counters | about twice per second |
| 8 | `stats_rcu()` | grace periods, readers and per-CPU observations | about twice per second |
| 9 | `stats_cpu()` | uptime, PMM, CPU and thread usage | once per second |

The monitor polls the active TTY and sleeps between updates.  It resets its
render timer when hidden so a live page can render promptly when selected.

The pages are observational.  They do not constitute stable management APIs,
and most sampled data is intended for human teaching rather than exact
accounting.

## Switching from input

On x86_64, the PS/2 keyboard IRQ handler recognises:

- Alt-0 for TTY 0;
- Alt-6 for configuration;
- Alt-7 for networking;
- Alt-8 for RCU;
- Alt-9 for CPU/system monitoring.

It refuses TTYs 6–9 until `monitor_ready` is true, avoiding a black page before
the renderer exists.

`kernel/serial_input.h` recognises an ESC byte followed by a digit as the
serial equivalent of Alt-digit.  A platform must actually feed received bytes
to `serial_input_process()` for this path to operate.  The current x86_64 COM1
IRQ does so.

Ordinary PS/2 characters are translated, including Shift and Caps Lock, but
they are not delivered to a TTY input consumer.  Pressing `c` clears the
framebuffer.  Control characters are recognised and then discarded at the
placeholder where a future line discipline or shell could receive them.

The serial input helper only echoes lowercase letters to serial outside an ESC
sequence.  It is a page-switch/debug aid, not a command parser.

## Statistics and consistency

All framebuffer printing is serialized by `print_lock`.  Monitor renderers
disable preemption across a frame so another local task does not split the
dashboard visually.

The CPU page also takes each remote runqueue lock while sampling `current` and
thread runtime counters.  This makes the per-core and per-thread percentages
use a consistent switching snapshot.

Other pages intentionally accept weaker snapshots.  For example, networking
statistics are published by the e1000 module and read without a general
snapshot lock.  Small cross-field inconsistencies are acceptable for a live
teaching display.

## Testing

`kernel/tty_test.c` checks six visibility cases:

- TTY 0 wildcard behaviour;
- matching and nonmatching monitor page bits;
- zero bitmask rejection on a monitor page;
- uninitialised rejection.

Enable it with `CONFIG_TTY_TEST=1`.  It runs in a kernel thread and reports
through serial.

The test does not verify:

- pixel correctness;
- page redraw contents;
- cursor restoration;
- concurrent printing while switching;
- keyboard/serial input delivery;
- monitor data accuracy;
- ARM64 framebuffer integration.

There is no dedicated default Make/CI gate for the TTY test.

## Exercises

1. Move cursor position into each TTY and restore it on switch.
2. Add true row scrolling and optional scrollback.
3. Turn the unused input queue into a bounded producer/consumer API.
4. Add a canonical line discipline with erase, kill, EOF and signal hooks.
5. Separate dashboard pages from process terminal semantics in the naming/API.
6. Add an offscreen-render test and a QEMU screenshot or framebuffer checksum
   gate.
