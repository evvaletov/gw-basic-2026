# Debug runs

The interpreter has a scripted line monitor on POSIX, DOS16 and DOS32. It can
step through statements and GOSUBs, stop at breakpoints, watch variables and
constant-index array elements, and export named inspection checkpoints.

```sh
build/gwbasic --debug-script examples/debug-media/commands.txt --debug-output trace.txt examples/debug-media/read.bas
```

This example reads `exchange.txt` in the working directory. Copy the fixture
from `examples/debug-media/exchange.txt`, or export it from the FORTRAN example
as described in the [coordination handoff](fortran-debug-media-handoff.md).
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
copies managed by the native runtime: the mapping and buffer display do not
claim pointer aliasing or synchronise variable changes into the record buffer.

`checkpoint NAME` / `snapshot NAME` write immutable log entries containing the
current registered watches and bounded state/file views. They do not restore
execution, capture all memory or replay external effects. EOF resumes and
turns off debugging. Unsupported commands produce debugger diagnostics.

The monitor supports the interpreter only. Compiled BASIC's runtime has no debug
hook; source maps and instrumentation are future work. BASIC still has no drum,
punch-card or cassette-image backend. External clocks, sound and event devices
continue during pauses; debugging a timing-sensitive program can affect its
behaviour. The current frontend is scripted text; interactive panes and richer
graphical source/file views remain in the coordinated backlog.

Run `python3 tests/test_debug.py` for the focused POSIX tests. After both DOS
builds, `python3 tests/run_dos_debug.py` checks actual execution and guest screen
preservation in FreeDOS/QEMU. It normally uses the pinned official boot floppy.
On a QEMU build without floppy support, supply `--system-disk /path/to/freedos.qcow2`;
the runner creates disposable overlays and verifies the base hash is unchanged.
