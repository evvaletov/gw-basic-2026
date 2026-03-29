# Roadmap

## The Big One

- **GW-BASIC 2026 Compiler** — ahead-of-time compilation of BASIC programs to
  native executables.  Because nothing says "premature optimization" like
  compiling a language designed for an interpreter running on a 4.77 MHz 8088.
  But we've come this far, so why not?  Likely approach: translate the token
  stream to C and lean on GCC/Clang for the heavy lifting.

## Completed

### Hardware I/O Simulator (v0.15.0)

Implemented in `portio.c` / `portio.h` following the `virmem.c` dispatch
pattern.  Emulates 8253 PIT channel 2 (speaker frequency), PPI port B
(speaker on/off with continuous tone via PulseAudio), CGA mode/color
registers, game port (joystick stub), and COM1 serial (transmitter-ready
stub).  Default: reads return 0xFF (floating bus), writes discarded.

Statements: `OUT`, `WAIT`, `MOTOR`.  Functions: `INP()`, `STICK()`, `STRIG()`.

Also in v0.15.0: filled remaining statement/function gaps — `RESET`,
`ENVIRON`/`ENVIRON$`, `ERDEV`/`ERDEV$`, `IOCTL`/`IOCTL$`, `LCOPY`,
`DATE$`/`TIME$` assignment, `CALL`/`CALLS`, `COM`.  All 144 defined
tokens are now handled (100% token coverage).

String space pool with compacting garbage collector (`strpool.c`),
replacing individual `malloc`/`free`.  32KB default pool, bump-pointer
allocation, compaction at statement boundaries.  `FRE()` returns actual
free space; `CLEAR n` resizes the pool.

### Jupyter Kernel (v0.15.0)

`gwbasickernel/` — Jupyter notebook kernel using the persistent subprocess
model.  GW-BASIC reads BASIC from stdin in piped mode (no banner, no prompts,
unbuffered stdout).  Sentinel protocol (`PRINT "<<<GWDONE>>>"`) delimits
output per cell.  State persists across cells.

- **Inline Sixel graphics** — `SCREEN 1`/`SCREEN 2` drawing commands render
  as inline PNG images in the notebook.  Pure-Python Sixel decoder (no PIL
  or Ghostscript dependency).
- **INPUT statement support** — when a program executes `INPUT`, the kernel
  requests input from the notebook front-end via the Jupyter stdin protocol.
- **Pygments syntax highlighting** — GW-BASIC lexer registered as a Pygments
  entry point for code cell highlighting.
- **Tab completion** for all GW-BASIC keywords.
- **Magic commands**: `%reset`, `%timeout`, `%new`.

Install: `pip install -e . && gwbasickernel-install --user`

## Next Up

### IDE Integration
- **JetBrains plugin (IntelliJ/CLion)** — full-featured language plugin with
  syntax highlighting, code completion, line number navigation, run
  configurations, debugger integration (breakpoints via `STOP`, variable
  inspection), structure view (line number outline), and error annotations.
- **VS Code extension** — language extension providing syntax highlighting
  (TextMate grammar), snippets, run/debug tasks, integrated terminal runner,
  and Language Server Protocol support for diagnostics and hover info.

## Known Limitations

- Maximum 256 variables, 64 arrays, 16 FOR nesting, 24 GOSUB nesting,
  16 WHILE nesting
- `CALL`/`CALLS` (machine code execution) raises Illegal function call
- `DATE$`/`TIME$` assignment accepted but does not modify the system clock
- Device stubs (`ERDEV`, `IOCTL`, `COM`, `LCOPY`) return defaults — no real
  device emulation
