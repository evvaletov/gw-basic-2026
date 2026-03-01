# Roadmap

## The Big One

- **GW-BASIC 2026 Compiler** — ahead-of-time compilation of BASIC programs to
  native executables.  Because nothing says "premature optimization" like
  compiling a language designed for an interpreter running on a 4.77 MHz 8088.
  But we've come this far, so why not?  Likely approach: translate the token
  stream to C and lean on GCC/Clang for the heavy lifting.

## Planned Features

- **DEF SEG / PEEK / POKE emulation** — a virtual address space for the BIOS data
  area and CGA screen buffer (B800:0000), so programs that directly twiddle
  screen memory or read the keyboard buffer actually work.  Not quite cycle-
  accurate, but enough to run most "tricks" from the 1980s magazines.
- **Hardware I/O simulator** — an optional emulation layer for `OUT`, `INP`,
  `WAIT`, `MOTOR`, and friends.  The idea is to provide a virtual PC peripheral
  bus so retro programs that talk to the speaker, joystick port, or parallel
  interface can do *something* useful instead of silently no-oping.  Think of it
  as a tiny museum exhibit for vintage BASIC hardware hacking.
- **BSAVE / BLOAD** — binary file save/load for screen buffers and data
- **PRINT USING edge cases** — `**` asterisk fill, `**$` combined, thousands
  separator with `,`, and `^^^^` scientific notation corner cases to match
  the original output formatting exactly
- **GET/PUT graphics** — sprite capture and blit for graphics mode; the
  framebuffer infrastructure is already in place, this is the missing piece
  for any BASIC program that does animation
- **TUI color support** — map GW-BASIC COLOR attributes to ANSI 16-color output
  in the TUI screen buffer
- **VIEW / WINDOW / PALETTE** — graphics viewport, coordinate mapping, and
  palette remapping
- **MBF format support for binary LOAD** — convert Microsoft Binary Format
  float constants when loading files saved by the original GWBASIC.EXE

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

- `PEEK`/`POKE` are stubs (POKE parses and discards, PEEK returns 0)
- Binary files use IEEE 754 floats, not MBF — files saved here are not
  byte-compatible with original GWBASIC.EXE binary format
- String garbage collection not implemented (uses `malloc`/`free` instead)
- Maximum 256 variables, 64 arrays, 16 FOR nesting, 24 GOSUB nesting,
  16 WHILE nesting
- Hardware I/O (`OUT`, `INP`, `WAIT`, `COM`, `MOTOR`) not yet implemented
