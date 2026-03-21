# Roadmap

## The Big One

- **GW-BASIC 2026 Compiler** — ahead-of-time compilation of BASIC programs to
  native executables.  Because nothing says "premature optimization" like
  compiling a language designed for an interpreter running on a 4.77 MHz 8088.
  But we've come this far, so why not?  Likely approach: translate the token
  stream to C and lean on GCC/Clang for the heavy lifting.

## Next Up

### Hardware I/O Simulator

An optional emulation layer for `OUT`, `INP`, `WAIT`, `MOTOR`, and friends,
following the established `virmem.c` dispatch-by-address pattern.

**New module:** `portio.c` / `portio.h` with `portio_inp(port)` /
`portio_out(port, value)` dispatch.

**Priority ports:**

| Port | Device | Emulation |
|------|--------|-----------|
| 0x42–0x43 | 8253 PIT channel 2 | Speaker frequency divisor (1193180 / divisor Hz) |
| 0x61 | PPI Port B | Speaker on/off (bits 0–1), routes to `snd_start_continuous()` / `snd_stop_continuous()` |
| 0x3D8 | CGA mode control | Mode register storage, feeds `gfx_init()` |
| 0x3D9 | CGA color select | Palette/background bits, feeds `gfx_palette_set()` |
| 0x201 | Game port | Joystick stub (returns 0xF0 = no buttons) |
| 0x3F8–0x3FE | COM1 serial | Minimal stub (LSR reports transmitter ready to prevent spin loops) |
| Default | — | Reads return 0xFF (floating bus), writes silently discarded |

**Statement/function changes:**

- `OUT port, value` → `portio_out(port, value)`
- `WAIT port, mask [, xor_mask]` → busy loop with `portio_inp()`, break check, timeout
- `MOTOR` → accept and silently ignore
- `INP(port)` → `portio_inp(port)` (currently returns 0)
- `STICK(n)` / `STRIG(n)` → joystick state from port 0x201

## IDE and Notebook Integration

- **Jupyter kernel for GW-BASIC** — a Jupyter Notebook kernel that runs
  GW-BASIC programs cell-by-cell, with rich output for `PRINT`, inline graphics
  rendering for drawing commands, and interactive `INPUT` via notebook widgets.
  Similar in spirit to [foxkernel](https://github.com/evvaletov/foxkernel).
- **JetBrains plugin (IntelliJ/CLion)** — full-featured language plugin with
  syntax highlighting, code completion, line number navigation, run
  configurations, debugger integration (breakpoints via `STOP`, variable
  inspection), structure view (line number outline), and error annotations.
- **VS Code extension** — language extension providing syntax highlighting
  (TextMate grammar), snippets, run/debug tasks, integrated terminal runner,
  and Language Server Protocol support for diagnostics and hover info.

## Known Limitations

- String garbage collection not implemented (uses `malloc`/`free` instead)
- Maximum 256 variables, 64 arrays, 16 FOR nesting, 24 GOSUB nesting,
  16 WHILE nesting
