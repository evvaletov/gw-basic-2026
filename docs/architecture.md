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

The interpreter mirrors the original GW-BASIC's internal pipeline, which -- like
most Microsoft interpreters of the era -- is a tight loop around three core
routines.  CRUNCH tokenizes source lines into a compact byte stream.  NEWSTT
dispatches each statement.  FRMEVL evaluates expressions.  All platform I/O
goes through a HAL vtable (`hal_ops_t`), so the core interpreter has no idea
whether it's talking to an ANSI terminal or a teletype from 1975.

In interactive mode, the TUI layer swaps in its own HAL function pointers and
redirects all output through a dynamically allocated screen buffer.  Rendering
itself is delegated back to the HAL via four ops (`tui_enter`, `tui_leave`,
`render_run`, `set_cursor_shape`): the POSIX backend emits ANSI escape
sequences, the DOS backend drives BIOS INT 10h directly, so the full-screen
editor works on bare FreeDOS without `ANSI.SYS`.  In piped mode the TUI stays
out of the way and the HAL writes straight to stdout.

## Module Map

| Module | Source | Original Assembly |
|--------|--------|--------------------|
| Tokenizer (CRUNCH/LIST) | `tokenizer.c` | GWMAIN.ASM |
| Expression evaluator | `eval.c` | GWEVAL.ASM |
| Execution loop + control flow | `interp.c` | BINTRP.ASM |
| TUI screen editor | `tui.c` | -- |
| Graphics engine | `graphics.c` | -- |
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
| Sound engine | `sound.c` | -- |
| Virtual memory (PEEK/POKE) | `virmem.c` | -- |
| Hardware I/O ports | `portio.c` | -- |
| String space pool + GC | `strpool.c` | GWEVAL.ASM (GETSPA/GARBAG) |
| AOT compiler analysis | `analysis.c` | -- |
| AOT compiler codegen | `codegen.c` | -- |
| Compiled program runtime | `gwrt.c` | -- |
| Platform abstraction | `hal_posix.c`, `hal_dos.c` | OEM*.ASM |

## Source Layout

```
src/         -- core interpreter + compiler (27 files)
include/     -- headers (18 files)
platform/    -- HAL backends (2 files)
gwbasickernel/ -- Jupyter notebook kernel (Python, 6 files)
tests/       -- 72 automated test programs, 4 classic interactive programs, compat harness
docs/        -- Sphinx documentation
```

## TUI Architecture

The TUI (`tui.c`) implements the classic GW-BASIC full-screen editor:

- **Screen buffer** -- `tui_cell_t *screen` is dynamically allocated at
  `rows × cols` (default 25×80, or full terminal size with `--full`).
  Each cell stores a character and color attribute, accessed via `TUI_CELL(r, c)`.
- **HAL interception** -- `tui_init()` swaps HAL function pointers so all
  existing PRINT/LIST/error output automatically goes through the screen buffer.
  No changes needed to `print.c`, `error.c`, or most of `interp.c`.
- **Line editor** -- `tui_read_line()` implements the defining GW-BASIC UX:
  free cursor movement with arrow keys, and pressing Enter on any screen line
  re-enters that line's content as BASIC input.
- **Function keys** -- F1-F10 with default GW-BASIC bindings, configurable via
  the `KEY n, "string"` statement. `KEY ON` shows the bar on the bottom row.
- **Break handling** -- SIGINT sets a flag checked each statement in the run loop.

## Ahead-of-Time Compiler

`gwbasic-compile` translates tokenized `.bas` programs to C source linked
against `libgwrt.a`:

```
.bas → gw_crunch() → analysis pass → C codegen → gcc → native binary
```

The **analysis pass** (`analysis.c`) collects variables, GOTO/GOSUB targets,
DATA literals, and DEFINT/DEFSNG/DEFDBL/DEFSTR type defaults.  When `--warn`
is active, it also tracks variable assignment vs. use context to detect
uninitialized variables, GOTO to nonexistent lines, and unreachable code.

The **codegen** (`codegen.c`) walks the token stream and emits C source:
variables become `static int16_t var_A_int`, control flow uses `goto L_100`
labels, and expressions are compiled inline using the same precedence-climbing
structure as `eval.c`.  Optimizations include constant folding, dead code
elimination, FOR step=1 elision, and fast-path expression emission.

The **`--safe` mode** modifies generated C to use overflow-checked integer
arithmetic (`gw_int_add/sub/mul` from `math_int.c`) instead of bare C
operators, and enhanced-diagnostic array/GOSUB runtime functions that report
variable names, subscript values, and BASIC line numbers on error.

The **runtime library** (`gwrt.c` / `libgwrt.a`) provides initialization,
DATA/READ support, GOSUB return-label stack, and wrappers around the existing
interpreter modules. It includes all interpreter code except the execution
loop, so compiled programs share the same string pool, GC, file I/O, and
graphics implementation.

## Design Decisions

### Relation to Original Assembly

Microsoft [released the original GW-BASIC source](https://github.com/microsoft/GW-BASIC)
in 2020 -- 43,771 lines of 8088 assembly spread across 43 `.ASM` files, complete
with Greg Whitten's comments and Neil Konzen's transcendental math routines
(which are, frankly, impressive for 16-bit fixed-point).  This reimplementation
uses that assembly as a reference, not as input to a transliterator -- the
algorithms are reimplemented in idiomatic C with modern data structures.

### Key Differences from the Original

- **IEEE 754 floating point** -- MBF (Microsoft Binary Format) conversion is used
  at the binary save/load boundary and for file I/O (CVI/CVS/CVD, MKI$/MKS$/MKD$),
  matching the original's on-disk format
- **Dynamic memory allocation** -- `malloc`/`free` instead of a 64KB segment layout
- **String space pool** -- 32KB contiguous pool with compacting GC at statement
  boundaries, matching the original's GETSPA/GARBAG approach
- **`setjmp`/`longjmp`** -- for error recovery, matching the original's stack reset
  behavior
- **ANSI terminal** -- TUI uses ANSI escape sequences and alternate screen buffer
  instead of direct CGA memory access
- **Dynamic screen buffer** -- allocated at runtime based on terminal size, rather
  than hardcoded to 25×80
