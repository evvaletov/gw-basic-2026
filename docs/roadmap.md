# Roadmap

## The Big One

- **GW-BASIC 2026 Compiler** — ahead-of-time compilation of BASIC programs to
  native executables.  Because nothing says "premature optimization" like
  compiling a language designed for an interpreter running on a 4.77 MHz 8088.
  But we've come this far, so why not?  Likely approach: translate the token
  stream to C and lean on GCC/Clang for the heavy lifting.

## Planned Features

- **Hardware I/O simulator** — an optional emulation layer for `OUT`, `INP`,
  `WAIT`, `MOTOR`, and friends.  The idea is to provide a virtual PC peripheral
  bus so retro programs that talk to the speaker, joystick port, or parallel
  interface can do *something* useful instead of silently no-oping.  Think of it
  as a tiny museum exhibit for vintage BASIC hardware hacking.
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

- String garbage collection not implemented (uses `malloc`/`free` instead)
- Maximum 256 variables, 64 arrays, 16 FOR nesting, 24 GOSUB nesting,
  16 WHILE nesting
- Hardware I/O (`OUT`, `INP`, `WAIT`, `COM`, `MOTOR`) not yet implemented
