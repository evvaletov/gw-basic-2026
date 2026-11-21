# Debug runs

The interpreter has a scripted line monitor on POSIX, DOS16 and DOS32. It can
step through statements and GOSUBs, stop at breakpoints, watch variables and
constant-index array elements, and export named inspection checkpoints.
The POSIX TUI uses this monitor from a second terminal. These features are
available in current source builds; the v0.18.3 downloads predate them.

## Interactive terminal controls

Build the interpreter, then run this from the repository root in the terminal
where the BASIC program should appear:

```sh
python3 tools/debug_tui.py run --session /tmp/gw-debug-1 tests/corpus/name.bas
```

In a second terminal, from the same repository:

```sh
python3 tools/debug_tui.py attach /tmp/gw-debug-1
```

The first terminal belongs to the BASIC program, including `INPUT`, keyboard
events and Sixel graphics. The second shows the entry source, current line,
token offset, GOSUB depth, values at the current stop and a scrollable event log.
Use Python 3.8 or newer with its standard `curses` module on a POSIX host;
the controller is tested on Linux. It does not run on DOS or Windows.
The source pane accepts ASCII files up to 1 MiB; tokenised saves and 8-bit
listings must be converted before using this controller.

| Key | Action |
| --- | --- |
| `s` / `c` | Step / continue |
| `b` | Enter a line or line:offset breakpoint |
| `w` / `u` | Add / remove a watch |
| `i` | Inspect a reference, `state`, or `file NUMBER OFFSET COUNT` |
| `p` | Name an inspection checkpoint |
| `:` | Enter any monitor command, including `clear` and `capabilities` |
| Up / Down | Scroll the recent events |
| Escape | Cancel command entry |
| `q` | Detach; BASIC resumes without further debugger stops |
| `x` | Cancel the BASIC run |

Monitor commands are available at a stop. After stepping an `INPUT` statement, enter
the answer in the program terminal. `q` and a controller interruption close
the command channel; the interpreter processes EOF at its next stop and
disables all breakpoints. Detachment does not terminate the BASIC program.
Use `x` in the inspector to cancel, including while BASIC is paused.
Keep the launcher running: forcibly killing it prevents cancellation and cleanup;
closing the remaining inspector can then resume BASIC through command-channel EOF.

Each run requires a new session directory. It contains a private command FIFO,
an entry-source snapshot, the event log and the exit status. Only one controller
can attach at a time. Once the first inspector exits or crashes, reopening is
read-only; start a fresh run to debug again. The viewer retains the latest 120
events and 16 values in memory; the complete log remains on disk until you
remove the session directory. Keep the inspector at least 50 columns wide and
18 rows high.
Only the most recent values that fit are visible. One-off inspections clear at
the next stop; registered watches refresh automatically and earlier values
remain in the event log.

The source pane highlights a line; the displayed offset identifies the exact
statement in token bytes. It is not a source-column highlight. The pane shows
the saved entry file and does not follow `LOAD`, `CHAIN` or program edits.
The controller sends the existing v1 line commands and never evaluates BASIC
expressions or implements file/device semantics itself.
Names are looked up literally. For example, `inspect TIMER` looks for a variable
named `TIMER`; it does not read the clock.

## Scripted monitor

```sh
build/gwbasic --debug-script examples/debug-media/commands.txt --debug-output trace.txt examples/debug-media/read.bas
```

This example reads `exchange.txt` in the working directory. Copy the fixture
from `examples/debug-media/exchange.txt`. It uses printable ASCII, one record per
line, for text exchange with FORTRAN 704.
Choose a new debug output filename on each run. Commands and output use separate
files; they do not enter the guest keyboard buffer or draw over its display.
On DOS the same options work with short filenames, e.g. `CMD.TXT` and `LOG.TXT`.

Read the [command/event contract](debug-contract-v1.md) for the complete grammar.
A short script could be:

```text
capabilities
watch A$
watch I%
break 40
continue
inspect file 1 0 16
checkpoint imported
step
continue
```

Stops identify BASIC line, token-byte offset and GOSUB depth. Colon-separated
statements and dispatched inline THEN/ELSE statements have separate offsets.
`break 40` stops at every dispatched statement on line 40; `break 40:12` stops
only at the exact token-byte offset printed by a previous run. Offsets can change
when source is edited. `step` enters GOSUB; `next`/`finish` are not implemented.

Watches preserve INTEGER/SINGLE/DOUBLE/STRING types and show array bounds.
Strings use a byte-exact hex display, including embedded zeroes. Absent scalars
show their defined BASIC defaults, without creating them. Absent arrays show
`array-not-allocated`, without triggering auto-DIM. Watch names are resolved
against the current DEF type unless a suffix is supplied. Expressions, variable
subscripts, functions and RNG/time calls are not evaluated.

`inspect state` shows variable/array counts, loop and GOSUB depths, and open
files. `inspect file NUMBER OFFSET COUNT` shows mode, logical byte position,
record length, input cursor, EOF state, FIELD mappings and up to 64 bytes of the
current random-file buffer. NUMBER is 1–15; OFFSET is 0–9999 in this initial
monitor. Inspection neither seeks nor reads the host file. FIELD variables are
copies managed by the native runtime. Inspection shows the mapping and buffer
without synchronising variable changes into the record buffer.

`checkpoint NAME` / `snapshot NAME` write immutable log entries containing the
current registered watches and bounded state/file views. They do not restore
execution, capture all memory or replay external effects. EOF resumes and
turns off debugging. Unsupported commands produce debugger diagnostics.

The monitor supports the interpreter only. Compiled BASIC's runtime has no debug
hook; source maps and instrumentation are future work. External clocks, sound and event devices
continue during pauses; debugging a timing-sensitive program can affect its
behaviour. Graphical source/file views, step-over and finish remain future work.
The shared v1 contract describes the underlying line
monitor; the POSIX controller above is a separate frontend over that protocol.

Run `python3 tests/test_debug.py` for the focused POSIX tests. After both DOS
builds, `python3 tests/run_dos_debug.py` checks execution and guest screen
preservation in FreeDOS/QEMU. It normally uses the pinned official boot floppy.
On a QEMU build without floppy support, supply `--system-disk /path/to/freedos.qcow2`;
the runner creates disposable overlays and verifies the base hash is unchanged.
For the terminal-controller tests, install `tests/requirements.txt` and
run `python3 tests/test_debug_tui.py`. They exercise separate pseudo-terminals,
guest input and Sixel output, calls, watches, file views, resizing, exclusive
attachment, detachment, signal cleanup and terminal restoration.
