# Roadmap

## Planned Features

- **BSAVE / BLOAD** — binary file save/load for screen buffers and data
- **DEF SEG** — memory segment declaration for PEEK/POKE/BSAVE/BLOAD
- **PRINT USING edge cases** — `**` asterisk fill, `**$` combined
- **Binary SAVE/LOAD** — Protected (,P) and tokenized binary formats
- **GET/PUT graphics** — sprite capture and blit for graphics mode
- **TUI color support** — map GW-BASIC COLOR attributes to ANSI 16-color output
- **INKEY$ extended keys** — return CHR$(0) + scan code for arrow keys and
  function keys, matching the original two-byte encoding
- **VIEW / WINDOW / PALETTE** — graphics viewport, coordinate mapping, and palette

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

- No binary/protected file format support (ASCII only)
- `PEEK`/`POKE` are stubs (POKE parses and discards, PEEK returns 0)
- String garbage collection not implemented (uses `malloc`/`free` instead)
- Maximum 256 variables, 64 arrays, 16 FOR nesting, 24 GOSUB nesting, 16 WHILE nesting
- Hardware I/O (OUT, INP, WAIT, COM, MOTOR) not implemented — no modern equivalent
