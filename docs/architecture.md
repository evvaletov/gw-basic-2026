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

The interpreter follows the original GW-BASIC pipeline. CRUNCH tokenizes source
lines into a compact byte stream, NEWSTT dispatches statements, and FRMEVL
evaluates expressions. Terminal operations use the hardware abstraction layer
(HAL) through `hal_ops_t`. File operations and optional PulseAudio output also
call their host libraries directly.

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
src/         -- core interpreter and compiler
include/     -- headers
platform/    -- POSIX and DOS HAL backends
gwbasickernel/ -- Jupyter notebook kernel (Python)
tests/       -- regression programs, classic interactive programs, compatibility harness
docs/        -- Sphinx documentation
```

## TUI Architecture

The TUI (`tui.c`) implements the classic GW-BASIC full-screen editor:

- Screen buffer: `tui_cell_t *screen` is dynamically allocated at
  `rows × cols` (default 25×80, or full terminal size with `--full`).
  Each cell stores a character and color attribute, accessed via `TUI_CELL(r, c)`.
- HAL interception: `tui_init()` swaps HAL function pointers so
  existing PRINT/LIST/error output automatically goes through the screen buffer.
  The output routines use the same HAL interface in both modes.
- Line editor: `tui_read_line()` supports
  free cursor movement with arrow keys, and pressing Enter on any screen line
  re-enters that line's content as BASIC input.
- Function keys: F1-F10 with default GW-BASIC bindings, configurable via
  the `KEY n, "string"` statement. `KEY ON` shows the bar on the bottom row.
- Break handling: on POSIX, SIGINT sets a flag checked during execution.

Program entry and prompted input use separate editing paths. Program entry
can read an existing screen row; prompted input keeps an answer buffer so
`INPUT` and `LINE INPUT` exclude the displayed prompt, including when the
answer wraps or scrolls.
Leading-semicolon input preserves the editing cursor on Enter. The reader
updates PRINT's column from the TUI cursor for both ordinary and semicolon
input, including wrapped/scrolled answers.

## Ahead-of-Time Compiler

`gwbasic-compile` reads ASCII `.bas` programs, tokenizes them, and emits C linked
against `libgwrt.a`:

```
.bas → gw_crunch() → analysis pass → C codegen → gcc → native binary
```

The analysis pass (`analysis.c`) collects variables, GOTO/GOSUB targets,
DATA literals, and DEFINT/DEFSNG/DEFDBL/DEFSTR type defaults.  When `--warn`
is active, it also tracks variable assignment vs. use context to detect
uninitialized variables, GOTO to nonexistent lines, and unreachable code.

Code generation (`codegen.c`) walks the token stream and emits C source:
variables become `static int16_t var_A_int`, control flow uses `goto L_100`
labels, and expressions are compiled inline using the same precedence-climbing
structure as `eval.c`.  Optimizations include constant folding, dead code
elimination and fast-path expression emission. Runtime stacks track active FOR
loops and GOSUB return sites. FOR validates its lexical NEXT before entering the
body; loop reentry and NEXT lookup stop at the current GOSUB frame, and RETURN
discards that frame's unfinished loops.

Programs containing `RESUME` also receive source-statement labels and retry/next
dispatch tables. The saved statement ID survives `longjmp` and is preserved
while the handler executes. Inline IF branches share their enclosing statement
until a colon starts a new one. A token-aware scanner locates continuation
delimiters without treating quoted colons or numeric payload bytes as syntax.
Programs without `RESUME` omit this tracking; dead-code removal remains enabled.

Default types are static only when declarations appear in the straight-line
entry prefix. Conditional or later DEFTYPE statements select separate numeric
and string variables/arrays at the same source location. `codegen_dynamic.inc`
emits tagged expressions, assignments, DIM, READ and FOR/NEXT storage access;
the main generator retains native labels, branches and error continuations.
`gwrt_value.c` supplies value conversions and builtins, while binary operations
share the interpreter's value arithmetic. Operands and arguments are evaluated
and checked in source order. Shared statement handlers use the same live storage
without copying scalars to and from generated C globals.

The runtime library enables the foreign-call bridge with `GW_NATIVE_RUNTIME`;
the standalone interpreter has no foreign declarations and omits that bridge.
An entry-local foreign-function callback makes `$EXTERN` available to delegated
expressions and DEF FN bodies. Generated wrappers copy string arguments to
bounded stack buffers and copy string results before those buffers expire.
Expression frames register live string descriptors so collection, including
`FRE("")`, relocates them with shared storage. Shared expression handlers and
DEF FN saved parameters participate when linked into the runtime library. Error
dispatch and shutdown discard roots whose C stack frames have unwound; host pool
pins remain untouched. Shutdown clears the callback and shared state.
Runtime-selected arrays use shared lookup even with `--inline-arrays`, and tagged
arithmetic retains its checks under `--fast-math`.

With `--inline-arrays`, eligible numeric arrays use cached pointers into the
runtime's storage. The cache generation changes when arrays are cleared or
erased. Access falls back to runtime lookup when a cache is absent or stale;
`--safe` always uses checked runtime access. The `DIM` statement still executes
at runtime and allocates the shared array storage.

The `--safe` mode modifies generated C to use overflow-checked integer
arithmetic (`gw_int_add/sub/mul` from `math_int.c`) instead of bare C
operators, and enhanced-diagnostic array/GOSUB runtime functions that report
variable names, subscript values, and BASIC line numbers on error.

The runtime library (`gwrt.c`, `gwrt_value.c` / `libgwrt.a`) provides initialization,
DATA/READ support, GOSUB return-label stack, and wrappers around the existing
interpreter modules, including the execution loop used for delegated program
operations such as `CHAIN`. Compiled programs share the string pool, garbage
collector, file I/O, and graphics implementation. Native code does not compile
the interpreter's timer or key event handlers.

## Design Decisions

### Relation to Original Assembly

Microsoft [released the original GW-BASIC source](https://github.com/microsoft/GW-BASIC)
in 2020. The C implementation uses that assembly as a reference for parsing,
evaluation, and program behavior.

### Key Differences from the Original

- IEEE 754 floating point: MBF (Microsoft Binary Format) conversion is used
  at the binary save/load boundary and for file I/O (CVI/CVS/CVD, MKI$/MKS$/MKD$),
  matching the original's on-disk format
- Dynamic memory allocation: variables, arrays, and program storage use the heap
- String space pool: compacting garbage collection, with an 8 KiB default for
  16-bit DOS and 32 KiB for other builds
- `setjmp`/`longjmp`: error recovery follows the original's stack reset
  behavior
- Terminal backends: the POSIX TUI uses ANSI escape sequences and the alternate
  screen buffer; the DOS TUI uses BIOS display services
- Dynamic screen buffer: allocated at startup as 25×80, or from the terminal
  dimensions with `--full`
