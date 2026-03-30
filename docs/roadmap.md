# Roadmap

## Completed

### Ahead-of-Time Compiler (v0.16.0)

`gwbasic-compile` translates tokenized .bas programs to C source, then
invokes GCC to produce native executables linked against `libgwrt.a`.

Pipeline: `.bas` → `gw_crunch()` → analysis pass → C codegen → `gcc` → native binary.

**69 of 69 eligible tests pass (100%).** Zero compile errors.

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

`gwbasickernel/` — Jupyter notebook kernel using the persistent subprocess
model with sentinel protocol.

- **Inline Sixel graphics** — pure-Python Sixel decoder renders SCREEN
  commands as inline PNG images in the notebook
- **INPUT statement support** via Jupyter stdin protocol
- **Pygments syntax highlighting** for code cells
- **Tab completion** for all GW-BASIC keywords
- **Magic commands**: `%reset`, `%timeout`, `%new`

Install: `pip install -e . && gwbasickernel-install --user`

## Next Up

### IDE Integration
- **VS Code extension** — syntax highlighting (TextMate grammar), snippets,
  run/debug tasks, integrated terminal runner
- **JetBrains plugin (IntelliJ/CLion)** — syntax highlighting, code completion,
  run configurations, debugger integration (breakpoints via `STOP`, variable
  inspection), structure view (line number outline)

## Known Limitations

- Maximum 256 variables, 64 arrays, 16 FOR nesting, 24 GOSUB nesting,
  16 WHILE nesting
- `CALL`/`CALLS` (machine code execution) raises Illegal function call
- `DATE$`/`TIME$` assignment accepted but does not modify the system clock
- Device stubs (`ERDEV`, `IOCTL`, `COM`, `LCOPY`) return defaults
