# Roadmap

## Completed

### Ahead-of-Time Compiler (v0.16.0)

`gwbasic-compile` translates tokenized .bas programs to C source, then
invokes GCC to produce native executables linked against `libgwrt.a`.

Pipeline: `.bas` → `gw_crunch()` → analysis pass → C codegen → `gcc` → native binary.

**63 of 63 eligible tests pass (100%)** via `tests/run_compiler_tests.sh`.
The harness only skips hardware-dependent tests (graphics/sound/timer)
and CHAIN/RUN target files that aren't standalone.  The compiler now
accepts unnumbered direct-mode programs by auto-numbering them.

Language coverage:

- All statements: PRINT, LET, IF/THEN/ELSE, GOTO, GOSUB/RETURN, FOR/NEXT,
  WHILE/WEND, ON GOTO/GOSUB, ON ERROR GOTO, RESUME/RESUME NEXT, DIM, DEF FN,
  SWAP, READ/DATA/RESTORE, INPUT/LINE INPUT, OPEN/CLOSE/PRINT#/INPUT#/WRITE#,
  FIELD/LSET/RSET/GET/PUT, BSAVE/BLOAD, SAVE/LOAD, CHAIN/COMMON, SCREEN,
  PSET/PRESET, COLOR/LOCATE/CLS, CIRCLE/DRAW/PAINT/PLAY, VIEW/WINDOW/PALETTE,
  POKE/OUT/WAIT, DEF SEG, RANDOMIZE, CLEAR, MID$ assignment, ERROR,
  KILL/NAME/FILES/SHELL/MKDIR/CHDIR/RMDIR, ENVIRON, LPRINT/LLIST, WIDTH, KEY
- All operators: `+` `-` `*` `/` `\` `MOD` `^` `AND` `OR` `XOR` `NOT` `EQV` `IMP`
  `>` `<` `=` `<=` `>=` `<>` (including string comparison via strcmp)
- All functions: math, string, file, conversion (CVI/CVS/CVD/MKI$/MKS$/MKD$),
  graphics (POINT/PMAP), system (FRE/ERR/ERL/TIMER/DATE$/TIME$/ENVIRON$/INKEY$)
- Token embedding for complex statements (PRINT USING, DEF FN, graphics,
  file I/O, MID$ assignment) with selective variable sync
- Division-by-zero detection, RNG matching (gw_rnd), ON ERROR GOTO via
  setjmp/longjmp

Optimizations:

- Constant folding (compile-time arithmetic on literals)
- Dead code elimination (skip statements after GOTO/END/STOP)
- FOR step=1 elision (var++ instead of step variable, simple comparison)
- Fast-path expression emitter (skip buffering for common case)
- Selective variable sync in delegated statements
- REM-line skip (no runtime check for comment-only lines)

### Hardware I/O Simulator (v0.15.0)

Implemented in `portio.c` / `portio.h` following the `virmem.c` dispatch
pattern.  Emulates 8253 PIT channel 2 (speaker frequency), PPI port B
(speaker on/off with continuous tone via PulseAudio), CGA mode/color
registers, game port (joystick stub), and COM1 serial (transmitter-ready
stub).  Default: reads return 0xFF (floating bus), writes discarded.

Also in v0.15.0: 100% token coverage (all 144 GW-BASIC tokens handled),
string space pool with compacting garbage collector, RESET, ENVIRON/ENVIRON$,
ERDEV/ERDEV$, IOCTL/IOCTL$, LCOPY, DATE$/TIME$ assignment, CALL, COM.

### Jupyter Kernel (v0.15.0)

`gwbasickernel/` -- Jupyter notebook kernel using the persistent subprocess
model with sentinel protocol.

- **Inline Sixel graphics** -- pure-Python Sixel decoder renders SCREEN
  commands as inline PNG images in the notebook
- **INPUT statement support** via Jupyter stdin protocol
- **Pygments syntax highlighting** for code cells
- **Tab completion** for all GW-BASIC keywords
- **Magic commands**: `%reset`, `%timeout`, `%new`

Install: `pip install -e . && gwbasickernel-install --user`

### Compiler Memory Safety (v0.17.0)

`--warn`, `--safe`, and `--safe=sanitize` flags for the ahead-of-time compiler.

- **`--warn`** -- static analysis: uninitialized variables, GOTO to nonexistent
  line, unreachable code detection.  Zero runtime cost.
- **`--safe`** (implies `--warn`) -- checked integer arithmetic via
  `gw_int_add/sub/mul/neg` (raises Overflow instead of wrapping), enhanced
  array bounds diagnostics with variable names and line numbers, GOSUB stack
  overflow diagnostics, ABS/SGN type-preserving codegen, string pool GC pinning
  infrastructure
- **`--safe=sanitize`** -- above plus `-fsanitize=address,undefined` passed to gcc

### DOS / FreeDOS Target (v0.17.0)

Cross-compiles to DOS using OpenWatcom V2.  Two targets:

- **16-bit real-mode** (`Makefile.dos16`): 128KB standalone MZ executable,
  MEDIUM memory model, far-heap TUI screen buffer, no DOS extender required
- **32-bit DOS/4GW** (`Makefile.dos`): 175KB LE executable, flat memory model,
  requires DOS4GW.EXE extender

Tested on FreeDOS 1.4 via QEMU.

## Next Up

### Compiler Optimization Flags
- **`--inline-arrays`** -- emit direct array indexing for statically-DIMmed arrays
  instead of runtime `gwrt_array_elem()` lookup
- **`-O0` through `-O3`** -- compiler-level optimization tiers mapping to different
  sets of codegen optimizations (constant folding, dead code elimination, FOR
  step=1 elision, fast-path expressions)

### Cross-Language Linking

Three levels of integration with C and Fortran.  Level 1 is implemented;
Level 2 is the natural follow-up; Level 3 is deferred unless a concrete
use case appears.

- **Level 1 -- Link BASIC objects into a larger C/Fortran project (done)**
  -- `gwbasic-compile prog.bas --emit-obj --main-name=run_basic` produces
  `prog.o` with the entry point renamed.  The host project links it
  alongside its own objects against `libgwrt`.  From Fortran, declare
  the entry with `bind(c)`.

- **Level 2 -- Foreign function declarations from BASIC**: extend the
  language with a `'$EXTERN NAME(ARGS) AS TYPE` pragma (or a new
  `EXTERNAL` statement) so BASIC code can call C functions directly.
  Type mapping: `INTEGER` <-> `int16_t`, `SINGLE` <-> `float`,
  `DOUBLE` <-> `double`, `STRING` <-> `char *` (NUL-terminated, owned
  by `gw_str_to_cstr`) or `gw_string_t` for richer interop.  Fortran
  callees must use `bind(c)`; legacy F77/F90 mangling out of scope --
  users write a thin C shim instead.

- **Level 3 (deferred) -- Embed individual BASIC SUBs/FUNCTIONs as
  C-callable functions**.  Compile each labeled SUB or DEF FN to a
  separate C function with a stable signature; emit a header so C
  drivers can call them.  Useful when BASIC is the configuration /
  rule-engine language for a larger application.  Bigger scope:
  needs export annotations, header generation, and a way to share
  state between calls.  Defer until a specific use case appears.

### IDE Integration
- **VS Code extension** -- syntax highlighting (TextMate grammar), snippets,
  run/debug tasks, integrated terminal runner
- **JetBrains plugin (IntelliJ/CLion)** -- syntax highlighting, code completion,
  run configurations, debugger integration (breakpoints via `STOP`, variable
  inspection), structure view (line number outline)

### Formatted I/O Extensions

Beyond the existing `PRINT` / `PRINT USING` / `PRINT#`, expose two
formatted-I/O styles familiar from neighbouring languages.  Both write
through the existing HAL output path so they work in interactive mode
and in compiled binaries.

- **FORTRAN-style `WRITE`** -- `WRITE (#unit, "(format-spec)") args`
  with Fortran format-spec language: `I5`, `F8.3`, `E12.4`, `A`, `X`,
  `/` (newline), repeat counts, slashes, parenthesized groups.  Useful
  for porting numerical code; Fortran formats are denser than
  PRINT USING.
- **C-style `PRINTF`** -- `PRINTF format$, arg, arg, ...` accepting C's
  `%d` / `%f` / `%e` / `%g` / `%s` / `%c` / `%x` / `%o` / width / precision
  / flags.  Cheaper to learn for users coming from C / Python.  Goes to
  stdout; `FPRINTF #unit, ...` for file output.

Both share an underlying formatter (probably a C function in
`libgwrt`) that the codegen calls directly; the interpreter tokenizes
and dispatches the same way.

### Numerical / Data Standard Library

Substantial scope -- treat as a separate sub-project, possibly a
companion repo.  All three modules build on top of GW-BASIC arrays
(or new dynamically-typed buffers via DEF SEG / virtual memory).
Likely written partly in BASIC and partly in C for the inner loops.

- **NumPy-style array module** -- `NDARRAY` type with shape, dtype,
  broadcasting; element-wise ops (`+`, `*`, `SIN`, `EXP`); reductions
  (`SUM`, `MIN`, `MAX`, `MEAN`); slicing; basic linear algebra (`MATMUL`,
  `INV`, `EIG`).  The existing single-typed BASIC arrays are a starting
  point; the new module needs a proper shape/dtype descriptor.
- **DataFrame module (pandas-like)** -- column-oriented table with
  named columns and heterogeneous dtypes; CSV / TSV load and save;
  filter, sort, group-by, aggregate, join.  Builds on the array
  module.
- **Plotting module (matplotlib-like)** -- high-level wrappers
  (`PLOT x, y`, `SCATTER`, `BAR`, `HIST`) with axes, labels, legend,
  title.  Backend: existing CGA / Sixel rendering for terminals; PNG
  output via stb_image_write or libpng for files; SVG as a third
  backend that needs no library.  Output format selectable
  (`SET BACKEND` or per-call argument).

Each module wants its own design pass before implementation; the
sketches above are the rough shapes.

## Known Limitations

- Static caps -- 32-bit / Linux builds: 1024 variables, 256 arrays,
  64 FOR nesting, 128 GOSUB nesting, 64 WHILE nesting.  16-bit real-mode
  DOS keeps the original modest caps (256 / 64 / 16 / 24 / 16) because
  the MEDIUM model has a single 64KB DGROUP for all static data.
- `CALL`/`CALLS` (machine code execution) raises Illegal function call
- `DATE$`/`TIME$` assignment shifts the program's view of the clock via
  a process-local offset; the OS time is unaffected (setting the OS
  clock would require root)
- Device stubs (`ERDEV`, `IOCTL`, `COM`, `LCOPY`) return defaults
