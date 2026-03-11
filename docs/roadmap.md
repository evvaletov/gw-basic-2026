# Roadmap

## The Big One

- **GW-BASIC 2026 Compiler** — ahead-of-time compilation of BASIC programs to
  native executables.  Because nothing says "premature optimization" like
  compiling a language designed for an interpreter running on a 4.77 MHz 8088.
  But we've come this far, so why not?  Likely approach: translate the token
  stream to C and lean on GCC/Clang for the heavy lifting.

## Next Up (v0.14.0)

### MBF Binary File Compatibility

Make the binary tokenized file format fully compatible with original GWBASIC.EXE
files.  Currently, float constants in the token stream are stored as IEEE 754;
the original uses Microsoft Binary Format (MBF).

**Approach:** always store MBF on disk, always IEEE in memory.  Convert at the
`load_binary()`/`save_binary()` boundary in `program_io.c`.  A token-walking
function scans each line for `TOK_CONST_SNG` (0x1C, 4 bytes) and
`TOK_CONST_DBL` (0x1F, 8 bytes) and converts in-place using the existing
`gw_mbf_to_ieee_*` / `gw_ieee_to_mbf_*` routines.  The walker must skip string
literals, REM/DATA regions, and multi-byte tokens to avoid false positives.

This makes round-tripping lossless (MBF single has the same 24-bit mantissa as
IEEE single; MBF double's 56 bits cleanly contain IEEE double's 53 bits) and
enables loading authentic `.BAS` files from the 1980s.

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

### DRAW Command Fixes

The DRAW mini-language parser has three bugs and four missing features.

**Bugs to fix:**

1. **M command parsing** — the generic argument parser consumes digits that
   belong to M's x-coordinate, so `DRAW "M100,50"` moves to (0, 50).  Fix:
   skip the generic parser when the command is M.
2. **S (scale) semantics** — scale is used as a default distance instead of a
   multiplier.  `DRAW "U"` moves 4 pixels instead of 1.  Correct formula:
   `dist = (has_arg ? arg : 1) * scale / 4`.
3. **A (90° rotation)** — recognized but no-op.  Needs a rotation state
   variable and direction vector transform for U/D/L/R/E/F/G/H.

**Features to add:**

- `TA n` — arbitrary rotation angle (degrees, −360 to 360); requires
  multi-character command parsing and trigonometric direction vector rotation
- `=variable;` — numeric variable substitution in DRAW strings; requires
  passing variable lookup capability into `gfx_draw()`
- `X subexpr;` — execute sub-string from a string variable (recursive parse)
- Scale applied to relative M coordinates

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
