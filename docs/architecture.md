# Architecture

## Pipeline

```
Source text → Tokenizer (CRUNCH) → Token stream
                                      ↓
                              Expression evaluator (FRMEVL)
                                      ↓
                              Statement dispatcher (NEWSTT)
                                      ↓
                              TUI screen buffer (interactive)
                                      ↓
                              HAL (platform I/O)
```

The interpreter follows the original GW-BASIC's internal structure. Source lines
are tokenized by CRUNCH into a compact token stream. The NEWSTT loop dispatches
each statement, calling FRMEVL for expression evaluation. All platform I/O goes
through a HAL vtable (`hal_ops_t`), keeping the core interpreter portable.

When running interactively, the TUI layer intercepts HAL output calls
(`putch`, `puts`, `cls`, `locate`) and routes them through a dynamically
allocated screen buffer rendered via ANSI escape sequences. In piped mode the
TUI is not activated and the HAL writes directly to stdout.

## Module Map

| Module | Source | Original Assembly |
|--------|--------|--------------------|
| Tokenizer (CRUNCH/LIST) | `tokenizer.c` | GWMAIN.ASM |
| Expression evaluator | `eval.c` | GWEVAL.ASM |
| Execution loop + control flow | `interp.c` | BINTRP.ASM |
| TUI screen editor | `tui.c` | — |
| Graphics engine | `graphics.c` | — |
| Token/keyword tables | `tokens.c`, `tokens.h` | IBMRES.ASM |
| Error handling | `error.c` | GWDATA.ASM |
| Integer arithmetic | `math_int.c` | MATH1.ASM |
| Float ops + MBF conversion | `math_float.c` | MATH2.ASM |
| Transcendentals | `math_transcend.c` | MATH1.ASM |
| String functions | `strings.c` | BISTRS.ASM |
| PRINT / LPRINT | `print.c` | BINTRP.ASM |
| PRINT USING | `print_using.c` | BIPRTU.ASM |
| Variables + arrays | `vars.c`, `arrays.c` | GWMAIN.ASM |
| File I/O + random access | `fileio.c` | BIPTRG.ASM |
| Program I/O (SAVE/LOAD) | `program_io.c` | BIMISC.ASM |
| INPUT/LINE INPUT | `input.c` | BINTRP.ASM |
| Sound engine | `sound.c` | — |
| Platform abstraction | `hal_posix.c` | OEM*.ASM |

## Source Layout

```
src/         — core interpreter (20 files)
include/     — headers (12 files)
platform/    — HAL backends (1 file)
tests/       — test programs (54 .BAS files), compat test harness
docs/        — Sphinx documentation
```

## TUI Architecture

The TUI (`tui.c`) implements the classic GW-BASIC full-screen editor:

- **Screen buffer** — `tui_cell_t *screen` is dynamically allocated at
  `rows × cols` (default 25×80, or full terminal size with `--full`).
  Each cell stores a character and color attribute, accessed via `TUI_CELL(r, c)`.
- **HAL interception** — `tui_init()` swaps HAL function pointers so all
  existing PRINT/LIST/error output automatically goes through the screen buffer.
  No changes needed to `print.c`, `error.c`, or most of `interp.c`.
- **Line editor** — `tui_read_line()` implements the defining GW-BASIC UX:
  free cursor movement with arrow keys, and pressing Enter on any screen line
  re-enters that line's content as BASIC input.
- **Function keys** — F1-F10 with default GW-BASIC bindings, configurable via
  the `KEY n, "string"` statement. `KEY ON` shows the bar on the bottom row.
- **Break handling** — SIGINT sets a flag checked each statement in the run loop.

## Design Decisions

### Relation to Original Assembly

The original GW-BASIC source was
[released by Microsoft in 2020](https://github.com/microsoft/GW-BASIC) as 8088
assembly (43,771 lines across 43 `.ASM` files). This reimplementation uses that
assembly as a reference but is not a transpilation — it reimplements the
algorithms in idiomatic C with modern data structures.

### Key Differences from the Original

- **IEEE 754 floating point** — MBF (Microsoft Binary Format) conversion is only
  used for file I/O compatibility (CVI/CVS/CVD, MKI$/MKS$/MKD$)
- **Dynamic memory allocation** — `malloc`/`free` instead of a 64KB segment layout
- **malloc'd strings** — instead of a compacting garbage collector
- **`setjmp`/`longjmp`** — for error recovery, matching the original's stack reset
  behavior
- **ANSI terminal** — TUI uses ANSI escape sequences and alternate screen buffer
  instead of direct CGA memory access
- **Dynamic screen buffer** — allocated at runtime based on terminal size, rather
  than hardcoded to 25×80
