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

### Cross-Language Linking (Levels 1 & 2)

- **Level 1 (v0.17.0)** -- `gwbasic-compile prog.bas --emit-obj
  --main-name=run_basic` produces `prog.o` with a renamed entry point, so a
  host C/Fortran project can link BASIC objects alongside its own against
  `libgwrt`.  From Fortran, declare the entry with `bind(c)`.
- **Level 2 -- `'$EXTERN` FFI pragma** -- `'$EXTERN NAME(ARGTYPES) AS RET`
  declares a C function callable from compiled BASIC, with INTEGER/SINGLE/
  DOUBLE/STRING ⇄ C type coercion at the boundary.  Case-preserving C symbol,
  BASIC-legal call name.  See *Foreign Functions from BASIC* in
  getting-started.md; test at `tests/run_ffi_test.sh`.  Arbitrary-C-symbol
  aliasing and string-result comparison are follow-ups (git-bug).

Level 3 (export BASIC routines as C-callable) remains deferred -- see git-bug.

## Planned

Actionable planned work is tracked in **git-bug** (`git-bug bug`), grouped
by priority/theme labels rather than duplicated here.  Release and outreach
items (FreeDOS package, Show HN writeup, etc.) live in git-bug only; this
file keeps the shipped-feature history and the known limitations.  Current
dev highlights:

| Theme | Item | git-bug | Priority |
|-------|------|---------|----------|
| compiler | `$EXTERN` follow-ups -- aliasing, INSTR/WRITE dispatch, validation | `8329647` | P2 |
| compiler | `--inline-arrays` direct array indexing | `e6d977c` | P2 |
| compiler | `-O0..-O3` codegen optimization tiers | `fecc17f` | P2 |
| compiler | Level 3 -- export BASIC SUBs/FUNCs as C-callable (deferred) | `1b7d59c` | P2 |
| language | FORTRAN-style `WRITE` formatted I/O | `a6e99af` | P2 |
| language | C-style `PRINTF` / `FPRINTF` | `cd8750c` | P2 |
| ide | VS Code extension (+ JetBrains follow-up) | `32a637c` | P2 |
| stdlib | Numerical/Data stdlib -- NDArray + DataFrame + Plotting (sub-project) | `55a9d14` | P2 |

Recently shipped: Level 2 `'$EXTERN` FFI pragma (`56b96e0`, closed).

Run `git-bug bug show <id>` for the full design notes on any item.  The
numerical/data stdlib (`55a9d14`) is the main enabler for the
Jupyter-kernel data-analysis use case.

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
