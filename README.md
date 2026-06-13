# GW-BASIC 2026

A portable C reimplementation of Microsoft GW-BASIC, using the
[original 8088 assembly source](https://github.com/microsoft/GW-BASIC)
(released by Microsoft in 2020) as the authoritative reference.

This is not a transpilation -- it reimplements the algorithms in clean C11
with modern data structures while targeting bug-compatible behavior.

## Building

```bash
cmake -B build && cmake --build build
```

Requires a C11 compiler and CMake 3.10+. PulseAudio (`libpulse-simple`)
is optional -- detected at build time for `SOUND`/`BEEP`/`PLAY` support.

Builds three targets:
- `gwbasic` -- the interpreter
- `gwbasic-compile` -- the ahead-of-time compiler
- `libgwrt.a` -- runtime library for compiled programs

## Usage

Interactive mode launches the authentic GW-BASIC full-screen editor:

```
$ ./gwbasic
GW-BASIC 2026 0.18.0
(C) Eremey Valetov 2026. MIT License.
Based on Microsoft GW-BASIC assembly source.
Ok
PRINT 2+2
 4
Ok
```

Run a program file (ASCII or binary tokenized):

```bash
./gwbasic tests/programs/prime_sieve.bas
```

Compile to a native executable:

```bash
./gwbasic-compile tests/programs/fibonacci.bas -c --runtime .
```

## Ahead-of-Time Compiler

`gwbasic-compile` translates BASIC programs to C source, then invokes GCC
to produce native executables linked against `libgwrt.a`.

Pipeline: `.bas` → tokenizer → analysis → C codegen → `gcc` → native binary.

63 of 63 eligible test programs compile via `gwbasic-compile` and produce
output matching the interpreter's golden files. Run `bash
tests/run_compiler_tests.sh` to verify.

```bash
# Generate C source
./gwbasic-compile myprog.bas -o myprog.c

# Compile to executable (automatic)
./gwbasic-compile myprog.bas -c --runtime /path/to/gw-basic-2026
```

## Jupyter Kernel

A Jupyter notebook kernel for GW-BASIC with inline Sixel graphics
rendering, INPUT support, and Pygments syntax highlighting.

```bash
pip install -e .
gwbasickernel-install --user
jupyter notebook  # select "GW-BASIC 2026" kernel
```

## What Works

100% token coverage -- all 144 GW-BASIC tokens are implemented.

**Data types:** INTEGER (%), SINGLE (!), DOUBLE (#), STRING ($)

**Operators:** `+ - * / ^ \ MOD AND OR XOR EQV IMP NOT < = > <= >= <>`

**Statements:**

| Category | Statements |
|----------|------------|
| Output | PRINT, LPRINT, LLIST, PRINT USING, WRITE, CLS |
| Variables | LET, DIM, ERASE, SWAP, DEFINT/SNG/DBL/STR |
| Control flow | GOTO, GOSUB/RETURN, FOR/NEXT, IF/THEN/ELSE, WHILE/WEND, ON...GOTO/GOSUB |
| Input | INPUT, LINE INPUT, DATA/READ/RESTORE, INKEY$ |
| Program | RUN, CONT, STOP, END, NEW, LIST, CLEAR, AUTO, RENUM, DELETE, EDIT |
| Sequential I/O | OPEN, CLOSE, PRINT#, WRITE#, INPUT#, LINE INPUT# |
| Random-access I/O | FIELD, LSET, RSET, PUT, GET, CVI/CVS/CVD, MKI$/MKS$/MKD$ |
| Program I/O | SAVE (binary/ASCII), LOAD (auto-detects), MERGE, CHAIN, COMMON |
| Event trapping | ON TIMER(n) GOSUB, TIMER ON/OFF/STOP, ON KEY(n) GOSUB |
| Error handling | ON ERROR GOTO, RESUME, ERROR, ERR, ERL |
| User functions | DEF FN, RANDOMIZE |
| File management | KILL, NAME, FILES, MKDIR, RMDIR, CHDIR, SHELL, ENVIRON |
| Date/time | DATE$, TIME$, TIMER |
| Screen | LOCATE, COLOR, WIDTH, SCREEN, KEY ON/OFF/LIST |
| Graphics | PSET, PRESET, LINE, CIRCLE, DRAW, PAINT, GET/PUT (sprites), VIEW, WINDOW, PALETTE, PMAP |
| Sound | SOUND, BEEP, PLAY (MML) |
| Memory | DEF SEG, PEEK, POKE, BSAVE, BLOAD |
| Hardware I/O | OUT, INP, WAIT, MOTOR, STICK, STRIG |

## Tests

72 interpreter tests, 14 kernel tests, 63 compiler tests -- all passing.

```bash
bash tests/run_tests.sh              # interpreter
python -m gwbasickernel.test_kernel  # Jupyter kernel
```

## DOS / FreeDOS

Cross-compiles to DOS with OpenWatcom V2:

```bash
wmake -f Makefile.dos16   # 16-bit real-mode (128KB standalone, no extender)
wmake -f Makefile.dos     # 32-bit DOS/4GW  (175KB, requires DOS4GW.EXE)
```

The 16-bit build runs on FreeDOS, MS-DOS, and compatible systems without
a DOS extender.  See [Getting Started](docs/getting-started.md) for details.

## Documentation

Full Sphinx documentation in `docs/`:

```bash
cd docs && pip install -r requirements.txt && make html
```

## License

MIT License. See [LICENSE](LICENSE).
